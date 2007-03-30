#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "typedefs.h"
#include "smalloc.h"
#include "vec.h"
#include "domdec.h"
#include "nrnb.h"
#include "pbc.h"
#include "constr.h"
#include "mdatoms.h"
#include "names.h"
#include "pdbio.h"
#include "futil.h"
#include "pme.h"
#include "pull.h"
#include "gmx_wallcycle.h"

#ifdef GMX_MPI
#include <mpi.h>
#endif

#define DDRANK(dd,rank)    (rank)
#define DDMASTERRANK(dd)   (dd->masterrank)

typedef struct gmx_domdec_master {
  /* The cell boundaries */
  real **cell_x;
  /* The global charge group division */
  int  *ncg;     /* Number of home charge groups for each node */
  int  *index;   /* Index of nnodes+1 into cg */
  int  *cg;      /* Global charge group index */
  int  *nat;     /* Number of home atoms for each node. */
  int  *ibuf;    /* Buffer for communication */
} gmx_domdec_master_t;

typedef struct {
  /* The numbers of charge groups and atoms to send and receive */
  int nsend[DD_MAXICELL+2];
  int nrecv[DD_MAXICELL+2];
  /* The charge groups to send */
  int *index;
  int nalloc;
} gmx_domdec_ind_t;

typedef struct {
  real *cell_size;
  bool *bCellMin;
  real *cell_f;
  real *old_cell_f;
  real *cell_f_max0;
  real *cell_f_min1;
  real *bound_min;
  real *bound_max;
  bool bLimited;
} gmx_domdec_root_t;

#define DD_NLOAD_MAX 9

/* Here floats are accurate enough, since these variables
 * only influence the load balancing, not the actual MD results.
 */
typedef struct {
  int  nload;
  float *load;
  float sum;
  float max;
  float sum_m;
  float cvol_min;
  float mdf;
  float pme;
  int   flags;
} gmx_domdec_load_t;

typedef struct gmx_domdec_comm {
  /* The communication setup including the PME only nodes */
  bool bCartesianPP_PME;
  ivec ntot;
  int  pmedim;
  int  *pmenodes;
  int  *ddindex2simnodeid;

  /* The DD particle-particle nodes only */
  /* The communication setup within the communicator all */
#ifdef GMX_MPI
  MPI_Comm all;
#endif
  bool bCartesianPP;
  int  *ddindex2ddnodeid;

  /* How to communicate for constraints and vsites */
  bool bSendRecv2;
  
  /* Are there bonded interactions between charge groups? */
  bool bInterCGBondeds;
    
  /* The width of the communicated boundaries */
  real distance_min;
  real distance;

  /* Orthogonal vectors for triclinic cells */
  rvec v[DIM][DIM];

  /* The old location of the cell boundaries, to check cg displacements */
  rvec old_cell_x0;
  rvec old_cell_x1;

  /* The cell boundaries of neighboring cells for dynamic load balancing */
  real **cell_d1;
  real ***cell_d2;

  /* The indices to communicate */
  gmx_domdec_ind_t ind[DIM];

  /* Communication buffer for general use */
  int  *buf_int;
  int  nalloc_int;

  /* Communication buffers for local redistribution */
  int  **cggl_flag;
  int  cggl_flag_nalloc[DIM*2];
  rvec **cgcm_state;
  int  cgcm_state_nalloc[DIM*2];
  rvec *buf_vr;
  int  nalloc_vr;

  /* Cell sizes for dynamic load balancing */
  gmx_domdec_root_t *root;
  real cell_f0[DIM];
  real cell_f1[DIM];
  real cell_f_max0[DIM];
  real cell_f_min1[DIM];

  /* Stuff for load communication */
  bool bRecordLoad;
  gmx_domdec_load_t *load;
#ifdef GMX_MPI
  MPI_Comm *mpi_comm_load;
#endif
  /* Cycle counters */
  float cycl[ddCyclNr];
  int   cycl_n[ddCyclNr];
  /* Flop counter */
  bool bCountFlop;
  double flop;
  int    flop_n;
  /* Have we measured the load? */
  bool bHaveLoad;
  /* Have we printed the load at least once? */
  bool bFirstPrinted;
} gmx_domdec_comm_t;

/* The size per charge group of the cggl_flag buffer in gmx_domdec_comm_t */
#define DD_CGIBS 2

/* The flags for the cggl_flag buffer in gmx_domdec_comm_t */
#define DD_FLAG_NRCG  65535
#define DD_FLAG_FW(d) (1<<(16+(d)*2))
#define DD_FLAG_BW(d) (1<<(16+(d)*2+1))

#define CG_ALLOC_SIZE     1000

/* Cell permutation required to obtain consecutive charge groups
 * for neighbor searching.
 */
static const int cell_perm[3][4] = { {0,0,0,0},{1,0,0,0},{3,0,1,2} };

/* The DD cell order */
static const ivec dd_co[DD_MAXCELL] =
  {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1},{1,1,1}};

/* The 3D setup */
#define dd_c3n  8
#define dd_cp3n 4
static const ivec dd_cp3[dd_cp3n] = {{0,0,8},{1,3,6},{2,5,6},{3,5,7}};

/* The 2D setup */
#define dd_c2n  4
#define dd_cp2n 2
static const ivec dd_cp2[dd_cp2n] = {{0,0,4},{1,3,4}};

/* The 1D setup */
#define dd_c1n  2
#define dd_cp1n 1
static const ivec dd_cp1[dd_cp1n] = {{0,0,2}};

static int nstDDDump,nstDDDumpGrid;


#define DD_CELL_MARGIN       1.0001
#define DD_PRES_SCALE_MARGIN 1.02


/*
#define dd_index(n,i) ((((i)[ZZ]*(n)[YY] + (i)[YY])*(n)[XX]) + (i)[XX])

static void index2xyz(ivec nc,int ind,ivec xyz)
{
  xyz[XX] = ind % nc[XX];
  xyz[YY] = (ind / nc[XX]) % nc[YY];
  xyz[ZZ] = ind / (nc[YY]*nc[XX]);
}
*/

/* This order is required to minimize the coordinate communication in PME
 * which uses decomposition in the x direction.
 */
#define dd_index(n,i) ((((i)[XX]*(n)[YY] + (i)[YY])*(n)[ZZ]) + (i)[ZZ])

static void ddindex2xyz(ivec nc,int ind,ivec xyz)
{
  xyz[XX] = ind / (nc[YY]*nc[ZZ]);
  xyz[YY] = (ind / nc[ZZ]) % nc[YY];
  xyz[ZZ] = ind % nc[ZZ];
}

static int ddcoord2ddnodeid(gmx_domdec_t *dd,ivec c)
{
  int ddindex;
  int ddnodeid=-1;

  ddindex = dd_index(dd->nc,c);
  if (dd->comm->bCartesianPP_PME) {
    ddnodeid = dd->comm->ddindex2ddnodeid[ddindex];
  } else if (dd->comm->bCartesianPP) {
#ifdef GMX_MPI
    MPI_Cart_rank(dd->comm->all,c,&ddnodeid);
#endif
  } else {
    ddnodeid = ddindex;
  }

  return ddnodeid;
}

int glatnr(gmx_domdec_t *dd,int i)
{
  int atnr;

  if (dd == NULL) {
    atnr = i + 1;
  } else {
    if (i >= dd->nat_tot_con)
      gmx_fatal(FARGS,"glatnr called with %d, which is larger than the local number of atoms (%d)",i,dd->nat_tot_con);
    atnr = dd->gatindex[i] + 1;
  }

  return atnr;
}

void dd_get_ns_ranges(gmx_domdec_t *dd,int icg,
		      int *jcg0,int *jcg1,ivec shift0,ivec shift1)
{
  int icell,d,dim;

  icell = 0;
  while (icg >= dd->icell[icell].cg1)
    icell++;

  if (icell == 0)
    *jcg0 = icg;
  else if (icell < dd->nicell)
    *jcg0 = dd->icell[icell].jcg0;
  else
    gmx_fatal(FARGS,"DD icg %d out of range: icell (%d) >= nicell (%d)",
	      icg,icell,dd->nicell);

  *jcg1 = dd->icell[icell].jcg1;

  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    shift0[dim] = dd->icell[icell].shift0[dim];
    shift1[dim] = dd->icell[icell].shift1[dim];
    if (dd->tric_dir[dim] || (dd->bGridJump && d > 0)) {
      /* A conservative approach, this can be optimized */
      shift0[dim] -= 1;
      shift1[dim] += 1;
    }
  }
}

void dd_sendrecv_int(const gmx_domdec_t *dd,
		     int ddim,int direction,
		     int *buf_s,int n_s,
		     int *buf_r,int n_r)
{
#ifdef GMX_MPI
  int rank_s,rank_r;
  MPI_Status stat;

  rank_s = dd->neighbor[ddim][direction==ddForward ? 0 : 1];
  rank_r = dd->neighbor[ddim][direction==ddForward ? 1 : 0];

  MPI_Sendrecv(buf_s,n_s*sizeof(int),MPI_BYTE,rank_s,0,
	       buf_r,n_r*sizeof(int),MPI_BYTE,rank_r,0,
	       dd->comm->all,&stat);
#endif
}

void dd_sendrecv_rvec(const gmx_domdec_t *dd,
		      int ddim,int direction,
		      rvec *buf_s,int n_s,
		      rvec *buf_r,int n_r)
{
#ifdef GMX_MPI
  int rank_s,rank_r;
  MPI_Status stat;

  rank_s = dd->neighbor[ddim][direction==ddForward ? 0 : 1];
  rank_r = dd->neighbor[ddim][direction==ddForward ? 1 : 0];

  MPI_Sendrecv(buf_s[0],n_s*sizeof(rvec),MPI_BYTE,rank_s,0,
	       buf_r[0],n_r*sizeof(rvec),MPI_BYTE,rank_r,0,
	       dd->comm->all,&stat);
#endif
}

void dd_sendrecv2_rvec(const gmx_domdec_t *dd,
		       int ddim,
		       rvec *buf_s_fw,int n_s_fw,
		       rvec *buf_r_fw,int n_r_fw,
		       rvec *buf_s_bw,int n_s_bw,
		       rvec *buf_r_bw,int n_r_bw)
{
#ifdef GMX_MPI
  int rank_fw,rank_bw,nreq;
  MPI_Request req[4];
  MPI_Status  stat[4];

  rank_fw = dd->neighbor[ddim][0];
  rank_bw = dd->neighbor[ddim][1];

  if (!dd->comm->bSendRecv2) {
    /* Try to send and receive in two directions simultaneously.
     * Should be faster, especially on machines
     * with full 3D communication networks.
     * However, it could be that communication libraries are
     * optimized for MPI_Sendrecv and non-blocking MPI calls
     * are slower.
     * SendRecv2 can be turned on with the env.var. GMX_DD_SENDRECV2
     */
    nreq = 0;
    if (n_r_fw)
      MPI_Irecv(buf_r_fw[0],n_r_fw*sizeof(rvec),MPI_BYTE,
		rank_bw,0,dd->comm->all,&req[nreq++]);
    if (n_r_bw)
      MPI_Irecv(buf_r_bw[0],n_r_bw*sizeof(rvec),MPI_BYTE,
		rank_fw,1,dd->comm->all,&req[nreq++]);
    if (n_s_fw)
      MPI_Isend(buf_s_fw[0],n_s_fw*sizeof(rvec),MPI_BYTE,
		rank_fw,0,dd->comm->all,&req[nreq++]);
    if (n_s_bw)
      MPI_Isend(buf_s_bw[0],n_s_bw*sizeof(rvec),MPI_BYTE,
		rank_bw,1,dd->comm->all,&req[nreq++]);
    if (nreq)
      MPI_Waitall(nreq,req,stat);
  } else {
    /* Communicate in two ordered phases.
     * This is slower, even on a dual-core Opteron cluster
     * with a single full-duplex network connection per machine.
     */
    /* Forward */
    MPI_Sendrecv(buf_s_fw[0],n_s_fw*sizeof(rvec),MPI_BYTE,rank_fw,0,
		 buf_r_fw[0],n_r_fw*sizeof(rvec),MPI_BYTE,rank_bw,0,
		 dd->comm->all,&stat[0]);
    /* Backward */
    MPI_Sendrecv(buf_s_bw[0],n_s_bw*sizeof(rvec),MPI_BYTE,rank_bw,0,
		 buf_r_bw[0],n_r_bw*sizeof(rvec),MPI_BYTE,rank_fw,0,
		 dd->comm->all,&stat[0]);
  }
#endif
}

void dd_move_x(gmx_domdec_t *dd,matrix box,rvec x[],rvec buf[])
{
  int  ncell,nat_tot,n,dim,i,j;
  int  *index,*cgindex;
  gmx_domdec_comm_t *comm;
  gmx_domdec_ind_t *ind;
  rvec shift;

  comm = dd->comm;
  
  cgindex = dd->cgindex;

  ncell = 1;
  nat_tot = dd->nat_home;
  for(dim=0; dim<dd->ndim; dim++) {
    ind = &comm->ind[dim];
    index = ind->index;
    n = 0;
    for(i=0; i<ind->nsend[ncell]; i++) {
      if (dd->ci[dd->dim[dim]] == 0) {
	/* We need to shift the coordinates */
	copy_rvec(box[dd->dim[dim]],shift);
	for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	  rvec_add(x[j],shift,buf[n]);
	  n++;
	}
      } else {
	for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	  copy_rvec(x[j],buf[n]);
	  n++;
	}
      }
    }
    /* Send and receive the coordinates */
    dd_sendrecv_rvec(dd, dim, ddBackward,
		     buf,       ind->nsend[ncell+1],
		     x+nat_tot, ind->nrecv[ncell+1]);
    nat_tot += ind->nrecv[ncell+1];
    ncell += ncell;
  }
}

void dd_move_f(gmx_domdec_t *dd,rvec f[],rvec buf[],rvec *fshift)
{
  int  ncell,nat_tot,n,dim,i,j;
  int  *index,*cgindex;
  gmx_domdec_comm_t *comm;
  gmx_domdec_ind_t *ind;
  ivec vis;
  int  is;

  comm = dd->comm;
  
  cgindex = dd->cgindex;

  ncell = 1;
  nat_tot = dd->nat_home;
  n = 0;
  ncell = dd->ncell/2;
  nat_tot = dd->nat_tot;
  for(dim=dd->ndim-1; dim>=0; dim--) {
    ind = &comm->ind[dim];
    nat_tot -= ind->nrecv[ncell+1];
    /* Communicate the forces */
    dd_sendrecv_rvec(dd,dim,ddForward,
		     f+nat_tot, ind->nrecv[ncell+1],
		     buf,       ind->nsend[ncell+1]);
    index = ind->index;
    /* Add the received forces */
    n = 0;
    for(i=0; i<ind->nsend[ncell]; i++) {
      if (fshift && dd->ci[dd->dim[dim]] == 0) {
	clear_ivec(vis);
	vis[dd->dim[dim]] = 1;
	is = IVEC2IS(vis);
	for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	  rvec_inc(f[j],buf[n]);
	  /* Add this force to the shift force */
	  rvec_inc(fshift[is],buf[n]);
	  n++;
	}
      } else {
	for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	  rvec_inc(f[j],buf[n]);
	  n++;
	}
      }
    }
    ncell /= 2;
  }
}

static void dd_move_cellx(gmx_domdec_t *dd,matrix box)
{
  int  d,d1,dim,dim1,pos,i,j,k;
  rvec buf[8],extr_s[2],extr_r[2];
  real len;
  gmx_domdec_comm_t *comm;

  comm = dd->comm;

  comm->cell_d1[0][0] = comm->cell_f0[1];
  comm->cell_d1[0][1] = comm->cell_f1[1];
  if (dd->ndim >= 3) {
    comm->cell_d2[0][0][0] = comm->cell_f0[2];
    comm->cell_d2[0][0][1] = comm->cell_f1[2];
  }

  pos = 0;
  for(d=dd->ndim-2; d>=0; d--) {
    dim  = dd->dim[d];
    dim1 = dd->dim[d+1];
    /* To use less code we use an rvec to store two reals */
    buf[pos][0] = comm->cell_f0[dim1];
    buf[pos][1] = comm->cell_f1[dim1];
    pos++;
    extr_s[d][0] = comm->cell_f0[dim1];
    extr_s[d][1] = comm->cell_f1[dim1];

    if (d == 0 && dd->ndim >= 3) {
      buf[pos][0] = extr_s[1][0];
      buf[pos][1] = extr_s[1][1];
      pos++;
    }

    if (dd->nc[dim] > 2) {
      /* We only need to communicate the extremes in the forward direction */
      dd_sendrecv_rvec(dd,dim,ddForward,
		       extr_s+d,dd->ndim-d-1,
		       extr_r+d,dd->ndim-d-1);
      for(d1=d; d1<dd->ndim-1; d1++) {
	extr_s[d1][0] = max(extr_s[d1][0],extr_r[d1][0]);
	extr_s[d1][1] = min(extr_s[d1][1],extr_r[d1][1]);
      }
    }

    dd_sendrecv_rvec(dd,dim,ddBackward,buf,pos,buf+pos,pos);

    if (d == 1 || (d == 0 && dd->ndim == 3)) {
      for(i=d; i<2; i++) {
	comm->cell_d2[1-d][i][0] = buf[pos][0];
	comm->cell_d2[1-d][i][1] = buf[pos][1];
	pos++;
	extr_s[1][0] = max(extr_s[1][0],comm->cell_d2[1-d][i][0]);
	extr_s[1][1] = min(extr_s[1][1],comm->cell_d2[1-d][i][1]);
      }
    }
    if (d == 0) {
      comm->cell_d1[1][0] = buf[pos][0];
      comm->cell_d1[1][1] = buf[pos][1];
      pos++;
      extr_s[0][0] = max(extr_s[0][0],comm->cell_d1[1][0]);
      extr_s[0][1] = min(extr_s[0][1],comm->cell_d1[1][1]);
    }
    if (d == 0 && dd->ndim >= 3) {
      extr_s[1][0] = max(extr_s[1][0],buf[pos][0]);
      extr_s[1][1] = min(extr_s[1][1],buf[pos][1]);
      pos++;
    }
  }

  if (dd->ndim >= 2) {
    dim = dd->dim[1];
    len = box[dim][dim];
    for(i=0; i<2; i++) {
      for(k=0; k<2; k++)
	comm->cell_d1[i][k] *= len;
      dd->cell_ns_x0[dim] = min(dd->cell_ns_x0[dim],comm->cell_d1[i][0]);
      dd->cell_ns_x1[dim] = max(dd->cell_ns_x1[dim],comm->cell_d1[i][1]);
    }
  }
  if (dd->ndim >= 3) {
    dim = dd->dim[2];
    len = box[dim][dim];
    for(i=0; i<2; i++)
      for(j=0; j<2; j++) {
	for(k=0; k<2; k++)
	  comm->cell_d2[i][j][k] *= len;
	dd->cell_ns_x0[dim] = min(dd->cell_ns_x0[dim],comm->cell_d2[i][j][0]);
	dd->cell_ns_x1[dim] = max(dd->cell_ns_x1[dim],comm->cell_d2[i][j][1]);
      }
  }
  for(d=1; d<dd->ndim; d++) {
    comm->cell_f_max0[d] = extr_s[d-1][0];
    comm->cell_f_min1[d] = extr_s[d-1][1];
  }
}

static void dd_bcast(gmx_domdec_t *dd,int nbytes,void *data)
{
#ifdef GMX_MPI
  MPI_Bcast(data,nbytes,MPI_BYTE,
	    DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_scatter(gmx_domdec_t *dd,int nbytes,void *src,void *dest)
{
#ifdef GMX_MPI
  MPI_Scatter(src,nbytes,MPI_BYTE,
	      dest,nbytes,MPI_BYTE,
	      DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_gather(gmx_domdec_t *dd,int nbytes,void *src,void *dest)
{
#ifdef GMX_MPI
  MPI_Gather(src,nbytes,MPI_BYTE,
	     dest,nbytes,MPI_BYTE,
	     DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_scatterv(gmx_domdec_t *dd,
			int *scounts,int *disps,void *sbuf,
			int rcount,void *rbuf)
{
#ifdef GMX_MPI
  MPI_Scatterv(sbuf,scounts,disps,MPI_BYTE,
	       rbuf,rcount,MPI_BYTE,
	       DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_gatherv(gmx_domdec_t *dd,
		       int scount,void *sbuf,
		       int *rcounts,int *disps,void *rbuf)
{
#ifdef GMX_MPI
  MPI_Gatherv(sbuf,scount,MPI_BYTE,
	      rbuf,rcounts,disps,MPI_BYTE,
	      DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_collect_cg(gmx_domdec_t *dd)
{
  gmx_domdec_master_t *ma;
  int buf2[2],*ibuf,i;

  ma = dd->ma;

  buf2[0] = dd->ncg_home;
  buf2[1] = dd->nat_home;
  if (DDMASTER(dd)) {
    ibuf = ma->ibuf;
  } else {
    ibuf = NULL;
  }
  /* Collect the charge group and atom counts on the master */
  dd_gather(dd,2*sizeof(int),buf2,ibuf);

  if (DDMASTER(dd)) {
    ma->index[0] = 0;
    for(i=0; i<dd->nnodes; i++) {
      ma->ncg[i] = ma->ibuf[2*i];
      ma->nat[i] = ma->ibuf[2*i+1];
      ma->index[i+1] = ma->index[i] + ma->ncg[i];
      
    }
    /* Make byte counts and indices */
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[i] = ma->ncg[i]*sizeof(int);
      ma->ibuf[dd->nnodes+i] = ma->index[i]*sizeof(int);
    }
    if (debug) {
      fprintf(debug,"Initial charge group distribution: ");
      for(i=0; i<dd->nnodes; i++)
	fprintf(debug," %d",ma->ncg[i]);
      fprintf(debug,"\n");
    }
  }
  
  /* Collect the charge group indices on the master */
  dd_gatherv(dd,
	     dd->ncg_home*sizeof(int),dd->index_gl,
	     DDMASTER(dd) ? ma->ibuf : NULL,
	     DDMASTER(dd) ? ma->ibuf+dd->nnodes : NULL,
	     DDMASTER(dd) ? ma->cg : NULL);

  dd->bMasterHasAllCG = TRUE;
}

void dd_collect_vec(gmx_domdec_t *dd,t_block *cgs,rvec *lv,rvec *v)
{
  gmx_domdec_master_t *ma;
  int  n,i,c,a,nalloc=0;
  rvec *buf=NULL;

  ma = dd->ma;

  if (!dd->bMasterHasAllCG)
    dd_collect_cg(dd);

  if (!DDMASTER(dd)) {
#ifdef GMX_MPI
    MPI_Send(lv,dd->nat_home*sizeof(rvec),MPI_BYTE,DDMASTERRANK(dd),
	     dd->rank,dd->comm->all);
#endif
  } else {
    /* Copy the master coordinates to the global array */
    n = DDMASTERRANK(dd);
    a = 0;
    for(i=ma->index[n]; i<ma->index[n+1]; i++)
      for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	copy_rvec(lv[a++],v[c]);
    
    for(n=0; n<dd->nnodes; n++) {
      if (n != dd->rank) {
	if (ma->nat[n] > nalloc) {
	  nalloc = over_alloc(ma->nat[n]);
	  srenew(buf,nalloc);
	}
#ifdef GMX_MPI
	MPI_Recv(buf,ma->nat[n]*sizeof(rvec),MPI_BYTE,DDRANK(dd,n),
		 n,dd->comm->all,MPI_STATUS_IGNORE);
#endif
	a = 0;
	for(i=ma->index[n]; i<ma->index[n+1]; i++)
	  for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	    copy_rvec(buf[a++],v[c]);
      }
    }
    sfree(buf);
  }
}

void dd_collect_state(gmx_domdec_t *dd,t_block *cgs,
		      t_state *state_local,t_state *state)
{
  int i;

  if (DDMASTER(dd)) {
    state->lambda = state_local->lambda;
    copy_mat(state_local->box,state->box);
    copy_mat(state_local->boxv,state->boxv);
    copy_mat(state_local->pcoupl_mu,state->pcoupl_mu);
    for(i=0; i<state_local->ngtc; i++)
      state->nosehoover_xi[i] = state_local->nosehoover_xi[i];
  }
  dd_collect_vec(dd,cgs,state_local->x,state->x);
  if (state_local->flags & STATE_HAS_V)
    dd_collect_vec(dd,cgs,state_local->v,state->v);
  if (state_local->flags & STATE_HAS_SDX)
    dd_collect_vec(dd,cgs,state_local->sd_X,state->sd_X);
}

static void dd_realloc_fr_cg(t_forcerec *fr,int nalloc)
{
  if (debug)
    fprintf(debug,"Reallocating forcerec: currently %d, required %d, allocating %d\n",fr->cg_nalloc,nalloc,over_alloc(nalloc));

  fr->cg_nalloc = over_alloc(nalloc);
  srenew(fr->cg_cm,fr->cg_nalloc);
  if (fr->solvent_opt != esolNO)
    srenew(fr->solvent_type,fr->cg_nalloc);
}

static void dd_realloc_state(t_state *state,rvec **f,rvec **buf,int nalloc)
{
  if (debug)
    fprintf(debug,"Reallocating state: currently %d, required %d, allocating %d\n",state->nalloc,nalloc,over_alloc(nalloc));

  state->nalloc = over_alloc(nalloc);
  srenew(state->x,state->nalloc);
  if (state->flags & STATE_HAS_V)
    srenew(state->v,state->nalloc);
  if (state->flags & STATE_HAS_SDX)
    srenew(state->sd_X,state->nalloc);
  
  srenew(*f,state->nalloc);
  srenew(*buf,state->nalloc);
}

static void dd_distribute_vec(gmx_domdec_t *dd,t_block *cgs,rvec *v,rvec *lv)
{
  gmx_domdec_master_t *ma;
  int  n,i,c,a,nalloc=0;
  rvec *buf=NULL;

  if (DDMASTER(dd)) {
    ma  = dd->ma;

    for(n=0; n<dd->nnodes; n++) {
      if (n != dd->rank) {
	if (ma->nat[n] > nalloc) {
	  nalloc = over_alloc(ma->nat[n]);
	  srenew(buf,nalloc);
	}
	/* Use lv as a temporary buffer */
	a = 0;
	for(i=ma->index[n]; i<ma->index[n+1]; i++)
	  for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	    copy_rvec(v[c],buf[a++]);
	if (a != ma->nat[n])
	  gmx_fatal(FARGS,"Internal error a (%d) != nat (%d)",a,ma->nat[n]);

#ifdef GMX_MPI
	MPI_Send(buf,ma->nat[n]*sizeof(rvec),MPI_BYTE,
		 DDRANK(dd,n),n,dd->comm->all);
#endif
      }
    }
    sfree(buf);
    n = DDMASTERRANK(dd);
    a = 0;
    for(i=ma->index[n]; i<ma->index[n+1]; i++)
      for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	copy_rvec(v[c],lv[a++]);
  } else {
#ifdef GMX_MPI
    MPI_Recv(lv,dd->nat_home*sizeof(rvec),MPI_BYTE,DDMASTERRANK(dd),
	     MPI_ANY_TAG,dd->comm->all,MPI_STATUS_IGNORE);
#endif
  }
}

static void dd_distribute_state(gmx_domdec_t *dd,t_block *cgs,
				t_state *state,t_state *state_local,
				rvec **f,rvec **buf)
{
  int  i;

  if (DDMASTER(dd)) {
    state_local->lambda = state->lambda;
    copy_mat(state->box,state_local->box);
    copy_mat(state->boxv,state_local->boxv);
    for(i=0; i<state_local->ngtc; i++)
      state_local->nosehoover_xi[i] = state->nosehoover_xi[i];
  }
  dd_bcast(dd,sizeof(real),&state_local->lambda);
  dd_bcast(dd,sizeof(state_local->box),state_local->box);
  dd_bcast(dd,sizeof(state_local->boxv),state_local->boxv);
  dd_bcast(dd,state_local->ngtc*sizeof(real),state_local->nosehoover_xi);
  if (dd->nat_home > state_local->nalloc)
    dd_realloc_state(state_local,f,buf,dd->nat_home);
  dd_distribute_vec(dd,cgs,state->x,state_local->x);
  if (state_local->flags & STATE_HAS_V)
    dd_distribute_vec(dd,cgs,state->v,state_local->v);
  if (state_local->flags & STATE_HAS_SDX)
    dd_distribute_vec(dd,cgs,state->sd_X,state_local->sd_X);
}

static char dim2char(int dim)
{
  char c='?';

  switch (dim) {
  case XX: c = 'x'; break;
  case YY: c = 'y'; break;
  case ZZ: c = 'z'; break;
  default: gmx_fatal(FARGS,"Unknown dim %d",dim);
  }

  return c;
}

static void write_dd_grid_pdb(char *fn,int step,gmx_domdec_t *dd,matrix box)
{
  rvec grid_s[2],*grid_r=NULL,cx,r;
  char fname[STRLEN],format[STRLEN];
  FILE *out;
  int  a,i,d,z,y,x;
  matrix tric;
  real vol;

  copy_rvec(dd->cell_x0,grid_s[0]);
  copy_rvec(dd->cell_x1,grid_s[1]);

  if (DDMASTER(dd))
    snew(grid_r,2*dd->nnodes);
  
  dd_gather(dd,2*sizeof(rvec),grid_s[0],DDMASTER(dd) ? grid_r[0] : NULL);

  if (DDMASTER(dd)) {
    for(d=0; d<DIM; d++) {
      for(i=0; i<DIM; i++) {
	if (d == i) {
	  tric[d][i] = 1;
	} else {
	  if (dd->nc[d] > 1)
	    tric[d][i] = box[i][d]/box[i][i];
	  else
	    tric[d][i] = 0;
	}
      }
    }
    sprintf(fname,"%s_%d.pdb",fn,step);
    sprintf(format,"%s%s\n",pdbformat,"%6.2f%6.2f");
    out = ffopen(fname,"w");
    gmx_write_pdb_box(out,box);
    a = 1;
    for(i=0; i<dd->nnodes; i++) {
      vol = dd->nnodes/(box[XX][XX]*box[YY][YY]*box[ZZ][ZZ]);
      for(d=0; d<DIM; d++)
	vol *= grid_r[i*2+1][d] - grid_r[i*2][d];
      for(z=0; z<2; z++)
	for(y=0; y<2; y++)
	  for(x=0; x<2; x++) {
	    cx[XX] = grid_r[i*2+x][XX];
	    cx[YY] = grid_r[i*2+y][YY];
	    cx[ZZ] = grid_r[i*2+z][ZZ];
	    mvmul(tric,cx,r);
	    fprintf(out,format,"ATOM",a++,"CA","GLY",' ',1+i,
		    10*r[XX],10*r[YY],10*r[ZZ],1.0,vol);
	  }
      for(d=0; d<DIM; d++) {
	for(x=0; x<4; x++) {
	  switch(d) {
	  case 0: y = 1 + i*8 + 2*x; break;
	  case 1: y = 1 + i*8 + 2*x - (x % 2); break;
	  case 2: y = 1 + i*8 + x; break;
	  }
	  fprintf(out,"%6s%5d%5d\n","CONECT",y,y+(1<<d));
	}
      }
    }
    fclose(out);
    sfree(grid_r);
  }
}

static void write_dd_pdb(char *fn,int step,char *title,t_atoms *atoms,
			 gmx_domdec_t *dd,int natoms,
			 rvec x[],matrix box)
{
  char fname[STRLEN],format[STRLEN];
  FILE *out;
  int  i,ii,resnr,c;
  real b;

  sprintf(fname,"%s_%d_n%d.pdb",fn,step,dd->sim_nodeid);

  sprintf(format,"%s%s\n",pdbformat,"%6.2f%6.2f");

  out = ffopen(fname,"w");

  fprintf(out,"TITLE     %s\n",title);
  gmx_write_pdb_box(out,box);
  for(i=0; i<natoms; i++) {
    ii = dd->gatindex[i];
    resnr = atoms->atom[ii].resnr;
    if (i < dd->nat_tot) {
      c = 0;
      while (i >= dd->cgindex[dd->ncg_cell[c+1]]) {
	c++;
      }
      b = c;
    } else if (i < dd->nat_tot_vsite) {
      b = dd->ncell;
    } else {
      b = dd->ncell + 1;
    }
    fprintf(out,format,"ATOM",(ii+1)%100000,
	    *atoms->atomname[ii],*atoms->resname[resnr],' ',(resnr+1)%10000,
	    10*x[i][XX],10*x[i][YY],10*x[i][ZZ],1.0,b);
  }
  fprintf(out,"TER\n");
  
  fclose(out);
}

static void make_dd_indices(gmx_domdec_t *dd,t_block *gcgs,int cg_start,
			    t_forcerec *fr)
{
  int cell,cg0,cg,cg_gl,a,a_gl;
  int *cell_ncg,*index_gl,*cgindex,*gatindex;
  gmx_ga2la_t *ga2la;
  bool bMakeSolventType;

  if (dd->nat_tot > dd->gatindex_nalloc) {
    dd->gatindex_nalloc = over_alloc(dd->nat_tot);
    srenew(dd->gatindex,dd->gatindex_nalloc);
  }
  
  if (fr->solvent_opt == esolNO) {
    /* Since all entries are identical, we can use the global array */
    fr->solvent_type = fr->solvent_type_global;
    bMakeSolventType = FALSE;
  } else {
    bMakeSolventType = TRUE;
  }

  cell_ncg   = dd->ncg_cell;
  index_gl   = dd->index_gl;
  cgindex    = gcgs->index;
  gatindex   = dd->gatindex;

  /* Make the local to global and global to local atom index */
  a = dd->cgindex[cg_start];
  for(cell=0; cell<dd->ncell; cell++) {
    if (cell == 0)
      cg0 = cg_start;
    else
      cg0 = cell_ncg[cell];
    for(cg=cg0; cg<cell_ncg[cell+1]; cg++) {
      cg_gl = index_gl[cg];
      for(a_gl=cgindex[cg_gl]; a_gl<cgindex[cg_gl+1]; a_gl++) {
	gatindex[a] = a_gl;
	ga2la = &dd->ga2la[a_gl];
	ga2la->cell = cell;
	ga2la->a    = a++;
      }
      if (bMakeSolventType)
	fr->solvent_type[cg] = fr->solvent_type_global[cg_gl];
    }
  }
}

static void clear_dd_indices(gmx_domdec_t *dd,int a_start)
{
  int i;

  /* Clear the indices without looping over all the atoms in the system */
  for(i=a_start; i<dd->nat_tot; i++)
    dd->ga2la[dd->gatindex[i]].cell = -1;

  dd_clear_local_vsite_indices(dd);

  if (dd->constraints)
    dd_clear_local_constraint_indices(dd);
}

static void check_grid_jump(int step,gmx_domdec_t *dd,matrix box)
{
  gmx_domdec_comm_t *comm;
  int  d,dim;
  real bfac;

  comm = dd->comm;
  
  for(d=1; d<dd->ndim; d++) {
    dim = dd->dim[d];
    bfac = box[dim][dim];
    if (dd->tric_dir[dim])
      bfac *= dd->skew_fac[dim];
    if ((comm->cell_f1[d] - comm->cell_f_max0[d])*bfac <  comm->distance ||
	(comm->cell_f0[d] - comm->cell_f_min1[d])*bfac > -comm->distance)
      gmx_fatal(FARGS,"Step %d: The domain decomposition grid has shifted too much in the %c-direction around cell %d %d %d\n",
		step,dim2char(dim),dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  }
}

static void set_tric_dir(gmx_domdec_t *dd,matrix box)
{
  int  d,i,j;
  rvec *v;
  real dep,skew_fac2;

  for(d=0; d<DIM; d++) {
    dd->tric_dir[d] = 0;
    for(j=d+1; j<DIM; j++) {
      if (box[j][d] != 0) {
	dd->tric_dir[d] = 1;
	if (dd->nc[j] > 1 && dd->nc[d] == 1)
	  gmx_fatal(FARGS,"Domain decomposition has not been implemented for box vectors that have non-zero components in directions that do not use domain decomposition: ncells = %d %d %d, box vector[%d] = %f %f %f",
		    dd->nc[XX],dd->nc[YY],dd->nc[ZZ],
		    j+1,box[j][XX],box[j][YY],box[j][ZZ]);
      }
    }
    
    /* Convert box vectors to orthogonal vectors for this dimension,
     * for use in distance calculations.
     * Set the trilinic skewing factor that translates
     * the thickness of a slab perpendicular to this dimension
     * into the real thickness of the slab.
     */
    if (dd->tric_dir[d]) {
      skew_fac2 = 1;
      v = dd->comm->v[d];
      if (d == XX || d == YY) {
	/* Normalize such that the "diagonal" is 1 */
	svmul(1/box[d+1][d+1],box[d+1],v[d+1]);
	for(i=0; i<d; i++)
	  v[d+1][i] = 0;
	skew_fac2 -= sqr(v[d+1][d]);
	if (d == XX) {
	  /* Normalize such that the "diagonal" is 1 */
	  svmul(1/box[d+2][d+2],box[d+2],v[d+2]);
	  for(i=0; i<d; i++)
	    v[d+2][i] = 0;
	  /* Make vector [d+2] perpendicular to vector [d+1],
	   * this does not affect the normalization.
	   */
	  dep = iprod(v[d+1],v[d+2])/norm2(v[d+1]);
	  for(i=0; i<DIM; i++)
	    v[d+2][i] -= dep*v[d+1][i];
	  skew_fac2 -= sqr(v[d+2][d]);
	}
	if (debug) {
	  fprintf(debug,"box[%d]  %.3f %.3f %.3f",
		  d,box[d][XX],box[d][YY],box[d][ZZ]);
	  for(i=d+1; i<DIM; i++)
	    fprintf(debug,"  v[%d] %.3f %.3f %.3f",
		    i,v[i][XX],v[i][YY],v[i][ZZ]);
	  fprintf(debug,"\n");
	}
      }
      dd->skew_fac[d] = sqrt(skew_fac2);
    } else {
      dd->skew_fac[d] = 1;
    }
  }
}

static bool dd_have_load(gmx_domdec_comm_t *comm)
{
  return (comm->bCountFlop ? (comm->flop_n > 0) : (comm->cycl_n[ddCyclF] > 0));
}

static float dd_force_load(gmx_domdec_comm_t *comm)
{
  return (comm->bCountFlop ? comm->flop : comm->cycl[ddCyclF]);
}

static void check_box_size(gmx_domdec_t *dd,matrix box)
{
  int d,dim;
  
  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    if (box[dim][dim]*dd->skew_fac[dim] <
	dd->nc[dim]*dd->comm->distance*DD_CELL_MARGIN)
      gmx_fatal(FARGS,"The %c-size of the box (%f) times the triclinic skew factor (%f) is smaller than the number of DD cells (%d) times the cut-off distance (%f)\n",
		dim2char(dim),box[dim][dim],dd->skew_fac[dim],
		dd->nc[dim],dd->comm->distance);
  }
}

static void set_dd_cell_sizes_slb(gmx_domdec_t *dd,matrix box,bool bMaster)
{
  int  d,j;
  real tot;

  for(d=0; d<DIM; d++) {
    if (dd->cell_load[d] == NULL || dd->nc[d] == 1) {
      /* Uniform grid */
      if (bMaster) {
	for(j=0; j<dd->nc[d]+1; j++)
	  dd->ma->cell_x[d][j] =      j*box[d][d]/dd->nc[d];
      } else {
	dd->cell_x0[d] = (dd->ci[d]  )*box[d][d]/dd->nc[d];
	dd->cell_x1[d] = (dd->ci[d]+1)*box[d][d]/dd->nc[d];
      }
    } else {
      /* Load balanced grid */
      tot = 0;
      for(j=0; j<dd->nc[d]; j++)
	tot += 1/dd->cell_load[d][j];
      
      if (bMaster) {
	dd->ma->cell_x[d][0] = 0;
	for(j=0; j<dd->nc[d]; j++)
	  dd->ma->cell_x[d][j+1] =
	    dd->ma->cell_x[d][j] + box[d][d]/(dd->cell_load[d][j]*tot);
      } else {
	dd->cell_x0[d] = 0;
	for(j=0; j<dd->ci[d]; j++)
	  dd->cell_x0[d] += box[d][d]/(dd->cell_load[d][j]*tot);
	dd->cell_x1[d] =
	  dd->cell_x0[d] +  box[d][d]/(dd->cell_load[d][dd->ci[d]]*tot);
      }
    }
  }
}

static void set_dd_cell_sizes_dlb(gmx_domdec_t *dd,matrix box,bool bDynamicBox,
				  bool bUniform,int step)
{
  gmx_domdec_comm_t *comm;
  gmx_domdec_root_t *root;
  int  d,d1,dim,dim1,i,j,pos,nmin,nmin_old;
  bool bRowMember,bRowRoot,bLimLo,bLimHi;
  real load_aver,load_i,imbalance,change,cutoff_f,cell_min,fac,space,halfway;
  real change_max = 0.05;
  real relax = 0.5;

  comm = dd->comm;

  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    bRowMember = TRUE;
    bRowRoot = TRUE;
    for(d1=d; d1<dd->ndim; d1++) {
      if (dd->ci[dd->dim[d1]] > 0) {
	if (d1 > d)
	  bRowMember = FALSE;
	bRowRoot = FALSE;
      }
    }
    
    root = &comm->root[d];
    if (bRowRoot) {
      /* Store the original boundaries */
      for(i=0; i<dd->nc[dim]+1; i++)
	root->old_cell_f[i] = root->cell_f[i];
      if (bUniform) {
	for(i=0; i<dd->nc[dim]; i++)
	  root->cell_size[i] = 1.0/dd->nc[dim];
      } else if (dd_have_load(comm)) {
	load_aver = comm->load[d].sum_m/dd->nc[dim];
	for(i=0; i<dd->nc[dim]; i++) {
	  /* Determine the relative imbalance of cell i */
	  load_i = comm->load[d].load[i*comm->load[d].nload+2];
	  imbalance = (load_i - load_aver)/load_aver;
	  /* Determine the change of the cell size using underrelaxtion */
	  change = -relax*imbalance;
	  /* Limit the amount of scaling */
	  if (change > change_max)
	    change = change_max;
	  else if (change < -change_max)
	    change = -change_max;
	  /* Set the optimal cell size */
	  root->cell_size[i] *= 1 + change;
	}
      }

      cutoff_f = comm->distance/box[dim][dim];
      cell_min = DD_CELL_MARGIN*cutoff_f;
      if (dd->tric_dir[dim])
	cell_min /= dd->skew_fac[dim];
      if (bDynamicBox && d > 0)
	cell_min *= DD_PRES_SCALE_MARGIN;

      if (d > 0 && !bUniform) {
	/* Make sure that the grid is not shifted too much */
	for(i=1; i<dd->nc[dim]; i++) {
	  root->bound_min[i] = root->cell_f_max0[i-1] + cell_min;
	  space = root->cell_f[i] - (root->cell_f_max0[i-1] + cell_min);
	  if (space > 0)
	    root->bound_min[i] += 0.5*space;
	  root->bound_max[i] = root->cell_f_min1[i] - cell_min;
	  space = root->cell_f[i] - (root->cell_f_min1[i] - cell_min);
	  if (space < 0)
	    root->bound_max[i] += 0.5*space;
	  if (debug)
	    fprintf(debug,
		    "dim %d boundary %d %.3f < %.3f < %.3f < %.3f < %.3f\n",
		    d,i,
		    root->cell_f_max0[i-1] + cell_min,
		    root->bound_min[i],root->cell_f[i],root->bound_max[i],
		    root->cell_f_min1[i] - cell_min);
	}
      }

      for(i=0; i<dd->nc[dim]; i++)
	root->bCellMin[i] = FALSE;
      nmin = 0;
      do {
	nmin_old = nmin;
	/* We need the total for normalization */
	fac = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  if (root->bCellMin[i] == FALSE)
	    fac += root->cell_size[i];
	}
	fac = (1 - nmin*cell_min)/fac;
	/* Determine the cell boundaries */
	root->cell_f[0] = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  if (root->bCellMin[i] == FALSE) {
	    root->cell_size[i] *= fac;
	    if (root->cell_size[i] < cell_min) {
	      root->bCellMin[i] = TRUE;
	      root->cell_size[i] = cell_min;
	      nmin++;
	    }
	  }
	  root->cell_f[i+1] = root->cell_f[i] + root->cell_size[i];
	}
      } while (nmin > nmin_old);

      /* Set the last boundary to exactly 1 */
      i = dd->nc[dim] - 1;
      root->cell_f[i+1] = 1;
      root->cell_size[i] = root->cell_f[i+1] - root->cell_f[i];
      if (root->cell_size[i] < cutoff_f)
	gmx_fatal(FARGS,"Step %d: the dynamic load balancing could not balance dimension %c: box size %f, triclinic skew factor %f, #cells %d, cut-off %f\n",
		  step,dim2char(dim),box[dim][dim],dd->skew_fac[dim],
		  dd->nc[dim],comm->distance);

      root->bLimited = (nmin > 0);
      
      if (!bUniform) {
	/* Check if the boundary did not displace more than halfway
	 * each of the cells it bounds, as this could cause problems,
	 * especially when the differences between cell sizes are large.
	 * If changes are applied, they will not make cells smaller
	 * than the cut-off, as we check all the boundaries which
	 * might be affected by a change and if the old state was ok,
	 * the cells will at most be shrunk back to their old size.
	 */
	for(i=1; i<dd->nc[dim]; i++) {
	  halfway = 0.5*(root->old_cell_f[i] + root->old_cell_f[i-1]);
	  if (root->cell_f[i] < halfway) {
	    root->cell_f[i] = halfway;
	    /* Check if the change also causes shifts of the next boundaries */
	    for(j=i+1; j<dd->nc[dim]; j++) {
	      if (root->cell_f[j] < root->cell_f[j-1] + cutoff_f)
		root->cell_f[j] =  root->cell_f[j-1] + cutoff_f;
	    }
	  }
	  halfway = 0.5*(root->old_cell_f[i] + root->old_cell_f[i+1]);
	  if (root->cell_f[i] > halfway) {
	    root->cell_f[i] = halfway;
	    /* Check if the change also causes shifts of the next boundaries */
	    for(j=i-1; j>=1; j--) {
	      if (root->cell_f[j] > root->cell_f[j+1] - cutoff_f)
		root->cell_f[j] = root->cell_f[j+1] - cutoff_f;
	    }
	  }
	}
      }

      if (d > 0) {
	/* Take care of the staggering of the cell boundaries */
	if (bUniform) {
	  for(i=0; i<dd->nc[dim]; i++) {
	    root->cell_f_max0[i] = root->cell_f[i];
	    root->cell_f_min1[i] = root->cell_f[i+1];
	  }
	} else {
	  for(i=1; i<dd->nc[dim]; i++) {
	    bLimLo = (root->cell_f[i] < root->bound_min[i]);
	    bLimHi = (root->cell_f[i] > root->bound_max[i]);
	    if (bLimLo && bLimHi) {
	      /* Both limits violated, try the best we can */
	      root->cell_f[i] = 0.5*(root->bound_min[i] + root->bound_max[i]);
	    } else if (bLimLo) {
	      root->cell_f[i] = root->bound_min[i];
	    } else if (bLimHi) {
	      root->cell_f[i] = root->bound_max[i];
	    }
	    if (bLimLo || bLimHi)
	      root->bLimited = TRUE;
	  }
	}
      }
      /* After the checks above, the cells should obey the cut-off
       * restrictions, but it does not hurt to check.
       */
      for(i=0; i<dd->nc[dim]; i++)
	if (root->cell_f[i+1] - root->cell_f[i] < cutoff_f)
	  fprintf(stderr,
		  "\nWARNING step %d: direction %c, cell %d too small: %f\n",
		  step,dim2char(dim),i,
		  (root->cell_f[i+1] - root->cell_f[i])
		  *box[dim][dim]*dd->skew_fac[dim]);
    
      pos = dd->nc[dim] + 1;
      /* Store the cell boundaries of the lower dimensions at the end */
      for(d1=0; d1<d; d1++) {
	root->cell_f[pos++] = comm->cell_f0[d1];
	root->cell_f[pos++] = comm->cell_f1[d1];
      }
    }
    if (bRowMember) {
      pos = dd->nc[dim] + 1 + d*2;
#ifdef GMX_MPI
      /* Each node would only need to know two fractions,
       * but it is probably cheaper to broadcast the whole array.
       */
      MPI_Bcast(root->cell_f,pos*sizeof(real),MPI_BYTE,
		0,dd->comm->mpi_comm_load[d]);
#endif
      /* Copy the fractions for this dimension from the buffer */
      comm->cell_f0[d] = root->cell_f[dd->ci[dim]  ];
      comm->cell_f1[d] = root->cell_f[dd->ci[dim]+1];
      pos = dd->nc[dim] + 1;
      for(d1=0; d1<=d; d1++) {
	if (d1 < d) {
	  /* Copy the cell fractions of the lower dimensions */
	  comm->cell_f0[d1] = root->cell_f[pos++];
	  comm->cell_f1[d1] = root->cell_f[pos++];
	}
	/* Set the cell dimensions */
	dim1 = dd->dim[d1];
	dd->cell_x0[dim1] = comm->cell_f0[d1]*box[dim1][dim1];
	dd->cell_x1[dim1] = comm->cell_f1[d1]*box[dim1][dim1];
      }
    }
  }
  
  /* Set the dimensions for which no DD is used */
  for(dim=0; dim<DIM; dim++) {
    if (dd->nc[dim] == 1) {
      dd->cell_x0[dim] = 0;
      dd->cell_x1[dim] = box[dim][dim];
    }
  }
}

static void set_dd_cell_sizes(gmx_domdec_t *dd,matrix box,bool bDynamicBox,
			      bool bUniform,bool bMaster,int step)
{
  int d;

  /* Copy the old cell boundaries for the cg displacement check */
  copy_rvec(dd->cell_x0,dd->comm->old_cell_x0);
  copy_rvec(dd->cell_x1,dd->comm->old_cell_x1);

  set_tric_dir(dd,box);

  if (DDMASTER(dd))
    check_box_size(dd,box);

  if (dd->bDynLoadBal && !bMaster) {
    set_dd_cell_sizes_dlb(dd,box,bDynamicBox,bUniform,step);
  } else {
    set_dd_cell_sizes_slb(dd,box,bMaster);
  }

  if (debug)
    for(d=0; d<DIM; d++)
      fprintf(debug,"cell_x[%d] %f - %f skew_fac %f\n",
	      d,dd->cell_x0[d],dd->cell_x1[d],dd->skew_fac[d]);
}

static void distribute_cg(FILE *fplog,int step,
			  matrix box,t_block *cgs,rvec pos[],
			  gmx_domdec_t *dd)
{
  gmx_domdec_master_t *ma;
  int **tmp_ind=NULL,*tmp_nalloc=NULL;
  int  i,icg,j,k,k0,k1,d;
  rvec invbox,cg_cm;
  ivec ind;
  real nrcg,inv_ncg,pos_d;
  atom_id *cgindex;

  ma = dd->ma;

  /* Set the cell boundaries */
  set_dd_cell_sizes(dd,box,FALSE,TRUE,TRUE,step);

  if (tmp_ind == NULL) {
    snew(tmp_nalloc,dd->nnodes);
    snew(tmp_ind,dd->nnodes);
    for(i=0; i<dd->nnodes; i++) {
      tmp_nalloc[i] = (cgs->nr/(dd->nnodes*CG_ALLOC_SIZE) + 2)*CG_ALLOC_SIZE;
      snew(tmp_ind[i],tmp_nalloc[i]);
    }
  }

  /* Clear the count */
  for(i=0; i<dd->nnodes; i++) {
    ma->ncg[i] = 0;
    ma->nat[i] = 0;
  }

  for(d=0; (d<DIM); d++)
    invbox[d] = divide(1,box[d][d]);

  cgindex = cgs->index;
  
  /* Compute the center of geometry for all charge groups */
  for(icg=0; icg<cgs->nr; icg++) {
    k0      = cgindex[icg];
    k1      = cgindex[icg+1];
    nrcg    = k1 - k0;
    if (nrcg == 1) {
      copy_rvec(pos[k0],cg_cm);
    }
    else {
      inv_ncg = 1.0/nrcg;
      
      clear_rvec(cg_cm);
      for(k=k0; (k<k1); k++)
	rvec_inc(cg_cm,pos[k]);
      for(d=0; (d<DIM); d++)
	cg_cm[d] = inv_ncg*cg_cm[d];
    }
    /* Put the charge group in the box and determine the cell index */
    for(d=DIM-1; d>=0; d--) {
      pos_d = cg_cm[d];
      if (dd->tric_dir[d] && dd->nc[d] > 1)
	for(j=d+1; j<DIM; j++)
	  pos_d -= cg_cm[j]*box[j][d]*invbox[j];
      while(pos_d >= box[d][d]) {
	pos_d -= box[d][d];
	rvec_dec(cg_cm,box[d]);
	for(k=k0; (k<k1); k++)
	  rvec_dec(pos[k],box[d]);
      }
      while(pos_d < 0) {
	pos_d += box[d][d];
	rvec_inc(cg_cm,box[d]);
	for(k=k0; (k<k1); k++)
	  rvec_inc(pos[k],box[d]);
      }
      /* This could be done more efficiently */
      ind[d] = 0;
      while(ind[d]+1 < dd->nc[d] && pos_d >= ma->cell_x[d][ind[d]+1])
	ind[d]++;
    }
    i = dd_index(dd->nc,ind);
    if (ma->ncg[i] == tmp_nalloc[i]) {
      tmp_nalloc[i] += CG_ALLOC_SIZE;
      srenew(tmp_ind[i],tmp_nalloc[i]);
    }
    tmp_ind[i][ma->ncg[i]] = icg;
    ma->ncg[i]++;
    ma->nat[i] += cgindex[icg+1] - cgindex[icg];
  }
  
  k1 = 0;
  for(i=0; i<dd->nnodes; i++) {
    ma->index[i] = k1;
    for(k=0; k<ma->ncg[i]; k++)
      ma->cg[k1++] = tmp_ind[i][k];
  }
  ma->index[dd->nnodes] = k1;

  for(i=0; i<dd->nnodes; i++)
    sfree(tmp_ind[i]);
  sfree(tmp_ind);
  sfree(tmp_nalloc);

  fprintf(fplog,"Charge group distribution:");
  for(i=0; i<dd->nnodes; i++)
    fprintf(fplog," %d",ma->ncg[i]);
  fprintf(fplog,"\n");
}

static void get_cg_distribution(FILE *fplog,int step,gmx_domdec_t *dd,
				t_block *cgs,matrix box,rvec pos[])
{
  gmx_domdec_master_t *ma=NULL;
  int i,cg_gl;
  int *ibuf,buf2[2] = { 0, 0 };

  clear_dd_indices(dd,0);

  if (DDMASTER(dd)) {
    ma = dd->ma;
    if (ma->ncg == NULL) {
      snew(ma->ncg,dd->nnodes);
      snew(ma->index,dd->nnodes+1);
      snew(ma->cg,cgs->nr);
      snew(ma->nat,dd->nnodes);
      snew(ma->ibuf,dd->nnodes*2);
      snew(ma->cell_x,DIM);
      for(i=0; i<DIM; i++)
	snew(ma->cell_x[i],dd->nc[i]+1);
    }      
    
    distribute_cg(fplog,step,box,cgs,pos,dd);
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[2*i]   = ma->ncg[i];
      ma->ibuf[2*i+1] = ma->nat[i];
    }
    ibuf = ma->ibuf;
  } else {
    ibuf = NULL;
  }
  dd_scatter(dd,2*sizeof(int),ibuf,buf2);

  dd->ncg_home = buf2[0];
  dd->nat_home = buf2[1];
  if (dd->ncg_home > dd->cg_nalloc || dd->cg_nalloc == 0) {
    dd->cg_nalloc = over_alloc(dd->ncg_home);
    srenew(dd->index_gl,dd->cg_nalloc);
    srenew(dd->cgindex,dd->cg_nalloc+1);
  }
  if (DDMASTER(dd)) {
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[i] = ma->ncg[i]*sizeof(int);
      ma->ibuf[dd->nnodes+i] = ma->index[i]*sizeof(int);
    }
  }

  dd_scatterv(dd,
	      DDMASTER(dd) ? ma->ibuf : NULL,
	      DDMASTER(dd) ? ma->ibuf+dd->nnodes : NULL,
	      DDMASTER(dd) ? ma->cg : NULL,
	      dd->ncg_home*sizeof(int),dd->index_gl);

  /* Determine the home charge group sizes */
  dd->cgindex[0] = 0;
  for(i=0; i<dd->ncg_home; i++) {
    cg_gl = dd->index_gl[i];
    dd->cgindex[i+1] =
      dd->cgindex[i] + cgs->index[cg_gl+1] - cgs->index[cg_gl];
  }

  if (debug) {
    fprintf(debug,"Home charge groups:\n");
    for(i=0; i<dd->ncg_home; i++) {
      fprintf(debug," %d",dd->index_gl[i]);
      if (i % 10 == 9) 
	fprintf(debug,"\n");
    }
    fprintf(debug,"\n");
  }

  dd->bMasterHasAllCG = TRUE;
}

static int compact_and_copy_vec_at(int ncg,int *move,
				   int *cgindex,
				   int nvec,int vec,
				   rvec *src,gmx_domdec_comm_t *comm)
{
  int m,icg,i,i0,i1,nrcg;
  int home_pos;
  int pos_vec[DIM*2];

  home_pos = 0;

  for(m=0; m<DIM*2; m++)
    pos_vec[m] = 0;

  i0 = 0;
  for(icg=0; icg<ncg; icg++) {
    i1 = cgindex[icg+1];
    m = move[icg];
    if (m == -1) {
      /* Compact the home array in place */
      for(i=i0; i<i1; i++)
	copy_rvec(src[i],src[home_pos++]);
    } else {
      /* Copy to the communication buffer */
      nrcg = i1 - i0;
      pos_vec[m] += 1 + vec*nrcg;
      for(i=i0; i<i1; i++)
	copy_rvec(src[i],comm->cgcm_state[m][pos_vec[m]++]);
      pos_vec[m] += (nvec - vec - 1)*nrcg;
    }
    i0 = i1;
  }

  return home_pos;
}

static int compact_and_copy_vec_cg(int ncg,int *move,
				   int *cgindex,
				   int nvec,rvec *src,gmx_domdec_comm_t *comm)
{
  int m,icg,i0,i1,nrcg;
  int home_pos;
  int pos_vec[DIM*2];

  home_pos = 0;

  for(m=0; m<DIM*2; m++)
    pos_vec[m] = 0;

  i0 = 0;
  for(icg=0; icg<ncg; icg++) {
    i1 = cgindex[icg+1];
    m = move[icg];
    if (m == -1) {
      /* Compact the home array in place */
      copy_rvec(src[icg],src[home_pos++]);
    } else {
      nrcg = i1 - i0;
      /* Copy to the communication buffer */
      copy_rvec(src[icg],comm->cgcm_state[m][pos_vec[m]]);
      pos_vec[m] += 1 + nrcg*nvec;
    }
    i0 = i1;
  }

  return home_pos;
}

static int compact_ind(int ncg,int *move,
		       int *index_gl,int *cgindex,
		       int *gatindex,gmx_ga2la_t *ga2la,
		       int *solvent_type)
{
  int cg,nat,a0,a1,a,a_gl;
  int home_pos;
  
  home_pos = 0;
  nat = 0;
  for(cg=0; cg<ncg; cg++) {
    a0 = cgindex[cg];
    a1 = cgindex[cg+1];
    if (move[cg] == -1) {
      /* Compact the home arrays in place.
       * Anything that can be done here avoids access to global arrays.
       */
      cgindex[home_pos] = nat;
      for(a=a0; a<a1; a++) {
	a_gl = gatindex[a];
	gatindex[nat] = a_gl;
	/* The cell number stays 0, so we don't need to set it */
	ga2la[a_gl].a = nat;
	nat++;
      }
      index_gl[home_pos] = index_gl[cg];
      if (solvent_type)
	solvent_type[home_pos] = solvent_type[cg];
      home_pos++;
    } else {
      /* Clear the global indices */
      for(a=a0; a<a1; a++) {
	a_gl = gatindex[a];
	ga2la[a_gl].cell = -1;
      }
    }
  }
  cgindex[home_pos] = nat;
  
  return home_pos;
}

static void print_cg_move(FILE *fplog,
			  gmx_domdec_t *dd,int step,int cg,int dim,int dir,
			  rvec cm_old,rvec cm_new,real pos_d)
{
  fprintf(fplog,"\nStep %d:\n",step);
  fprintf(fplog,"The charge group starting at atom %d moved more than the cut-off (%f) in direction %c\n",
	  glatnr(dd,dd->cgindex[cg]),dd->comm->distance,dim2char(dim));
  fprintf(fplog,"distance out of cell %f\n",
	  dir==1 ? pos_d - dd->cell_x1[dim] : pos_d - dd->cell_x0[dim]);
  fprintf(fplog,"Old coordinates: %8.3f %8.3f %8.3f\n",
	  cm_old[XX],cm_old[YY],cm_old[ZZ]);
  fprintf(fplog,"New coordinates: %8.3f %8.3f %8.3f\n",
	  cm_new[XX],cm_new[YY],cm_new[ZZ]);
  fprintf(fplog,"Old cell boundaries in direction %c: %8.3f %8.3f\n",
	  dim2char(dim),
	  dd->comm->old_cell_x0[dim],dd->comm->old_cell_x1[dim]);
  fprintf(fplog,"New cell boundaries in direction %c: %8.3f %8.3f\n",
	  dim2char(dim),
	  dd->cell_x0[dim],dd->cell_x1[dim]);
}

static void cg_move_error(FILE *fplog,
			  gmx_domdec_t *dd,int step,int cg,int dim,int dir,
			  rvec cm_old,rvec cm_new,real pos_d)
{
  print_cg_move(fplog, dd,step,cg,dim,dir,cm_old,cm_new,pos_d);
  print_cg_move(stderr,dd,step,cg,dim,dir,cm_old,cm_new,pos_d);
  gmx_fatal(FARGS,"A charge group move too far between two domain decomposition steps");
}

static int dd_redistribute_cg(FILE *fplog,int step,
			      gmx_domdec_t *dd,t_block *gcgs,
			      t_state *state,rvec **f,rvec **buf,
			      t_forcerec *fr,t_mdatoms *md,
			      t_nrnb *nrnb)
{
  int  *move;
  int  ncg[DIM*2],nat[DIM*2];
  int  c,i,cg,k,k0,k1,d,dim,dim2,dir,d2,d3,d4,cell_d;
  int  mc,cdd,nrcg,ncg_recv,nat_recv,nvs,nvr,nvec,vec;
  int  sbuf[2],rbuf[2];
  int  home_pos_cg,home_pos_at,ncg_stay_home,buf_pos;
  int  flag;
  bool bV,bSDX;
  ivec tric_dir,dev;
  real inv_ncg,pos_d;
  rvec *cg_cm,invbox,cell_x0,cell_x1,limit0,limit1,cm_new;
  atom_id *cgindex;
  gmx_domdec_comm_t *comm;

  comm  = dd->comm;
  cg_cm = fr->cg_cm;

  bV   = (state->flags & STATE_HAS_V);
  bSDX = (state->flags & STATE_HAS_SDX);

  if (dd->ncg_tot > comm->nalloc_int) {
    comm->nalloc_int = over_alloc(dd->ncg_tot);
    srenew(comm->buf_int,comm->nalloc_int);
  }
  move = comm->buf_int;

  /* Clear the count */
  for(c=0; c<dd->ndim*2; c++) {
    ncg[c] = 0;
    nat[c] = 0;
  }

  for(d=0; (d<DIM); d++) {
    invbox[d] = divide(1,state->box[d][d]);
    cell_x0[d] = dd->cell_x0[d];
    cell_x1[d] = dd->cell_x1[d];
    c = dd->ci[d] - 1;
    if (c < 0)
      c = dd->nc[d] - 1;
    limit0[d] = comm->old_cell_x0[d] - comm->distance;
    c = dd->ci[d] + 1;
    if (c >= dd->nc[d])
      c = 0;
    limit1[d] = comm->old_cell_x1[d] + comm->distance;
    if (dd->tric_dir[d] && dd->nc[d] > 1)
      tric_dir[d] = 1;
    else
      tric_dir[d] = 0;
  }

  cgindex = dd->cgindex;

  /* Compute the center of geometry for all home charge groups
   * and put them in the box and determine where they should go.
   */
  for(cg=0; cg<dd->ncg_home; cg++) {
    k0   = cgindex[cg];
    k1   = cgindex[cg+1];
    nrcg = k1 - k0;
    if (nrcg == 1) {
      copy_rvec(state->x[k0],cm_new);
    }
    else {
      inv_ncg = 1.0/nrcg;
      
      clear_rvec(cm_new);
      for(k=k0; (k<k1); k++)
	rvec_inc(cm_new,state->x[k]);
      for(d=0; (d<DIM); d++)
	cm_new[d] = inv_ncg*cm_new[d];
    }

    for(d=DIM-1; d>=0; d--) {
      if (dd->nc[d] > 1) {
	/* Determine the location of this cg in lattice coordinates */
	pos_d = cm_new[d];
	if (tric_dir[d])
	  for(d2=d+1; d2<DIM; d2++)
	    pos_d -= cm_new[d2]*state->box[d2][d]*invbox[d2];
	/* Put the charge group in the triclinic unit-cell */
	if (pos_d >= cell_x1[d]) {
	  if (pos_d >= limit1[d])
	    cg_move_error(fplog,dd,step,cg,d,1,cg_cm[cg],cm_new,pos_d);
	  dev[d] = 1;
	  if (dd->ci[d] == dd->nc[d] - 1) {
	    rvec_dec(cm_new,state->box[d]);
	    for(k=k0; (k<k1); k++)
	      rvec_dec(state->x[k],state->box[d]);
	  }
	} else if (pos_d < cell_x0[d]) {
	  if (pos_d < limit0[d])
	    cg_move_error(fplog,dd,step,cg,d,-1,cg_cm[cg],cm_new,pos_d);
	  dev[d] = -1;
	  if (dd->ci[d] == 0) {
	    rvec_inc(cm_new,state->box[d]);
	    for(k=k0; (k<k1); k++)
	      rvec_inc(state->x[k],state->box[d]);
	  }
	} else {
	  dev[d] = 0;
	}
      } else {
	/* Put the charge group in the rectangular unit-cell */
	while (cm_new[d] >= state->box[d][d]) {
	  rvec_dec(cm_new,state->box[d]);
	  for(k=k0; (k<k1); k++)
	    rvec_dec(state->x[k],state->box[d]);
	}
	while (cm_new[d] < 0) {
	  rvec_inc(cm_new,state->box[d]);
	  for(k=k0; (k<k1); k++)
	    rvec_inc(state->x[k],state->box[d]);
	}
      }
    }
    
    copy_rvec(cm_new,cg_cm[cg]);

    /* Determine where this cg should go */
    flag = 0;
    mc = -1;
    for(d=0; d<dd->ndim; d++) {
      dim = dd->dim[d];
      if (dev[dim] == 1) {
	flag |= DD_FLAG_FW(d);
	if (mc == -1)
	  mc = d*2;
      } else if (dev[dim] == -1) {
	flag |= DD_FLAG_BW(d);
	if (mc == -1) {
	  if (dd->nc[dim] > 2)
	    mc = d*2 + 1;
	  else
	    mc = d*2;
	}
      }
    }
    move[cg] = mc;
    if (mc >= 0) {
      if (ncg[mc]+1 > comm->cggl_flag_nalloc[mc]) {
	comm->cggl_flag_nalloc[mc] = over_alloc(ncg[mc]+1);
	srenew(comm->cggl_flag[mc],comm->cggl_flag_nalloc[mc]*DD_CGIBS);
      }
      comm->cggl_flag[mc][ncg[mc]*DD_CGIBS  ] = dd->index_gl[cg];
      /* We store the cg size in the lower 16 bits
       * and the place where the charge group should go
       * in the next 6 bits. This saves some communication volume.
       */
      comm->cggl_flag[mc][ncg[mc]*DD_CGIBS+1] = nrcg | flag;
      ncg[mc] += 1;
      nat[mc] += nrcg;
    }
  }

  inc_nrnb(nrnb,eNR_CGCM,dd->nat_home);
  inc_nrnb(nrnb,eNR_RESETX,dd->ncg_home);

  nvec = 1;
  if (bV)
    nvec++;
  if (bSDX)
    nvec++;

  /* Make sure the communication buffers are large enough */
  for(mc=0; mc<dd->ndim*2; mc++) {
    nvr = ncg[mc] + nat[mc]*nvec;
    if (nvr>comm->cgcm_state_nalloc[mc]) {
      comm->cgcm_state_nalloc[mc] = over_alloc(nvr);
      srenew(comm->cgcm_state[mc],comm->cgcm_state_nalloc[mc]);
    }
  }

  /* Recalculating cg_cm might be cheaper than communicating,
   * but that could give rise to rounding issues.
   */
  home_pos_cg =
    compact_and_copy_vec_cg(dd->ncg_home,move,cgindex,
			    nvec,cg_cm,comm);

  vec = 0;
  home_pos_at =
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->x,comm);
  if (bV)
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->v,comm);
  if (bSDX)
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->sd_X,comm);
  
  compact_ind(dd->ncg_home,move,
	      dd->index_gl,dd->cgindex,
	      dd->gatindex,dd->ga2la,
	      fr->solvent_opt==esolNO ? NULL : fr->solvent_type);

  ncg_stay_home = home_pos_cg;
  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    ncg_recv = 0;
    nat_recv = 0;
    nvr      = 0;
    for(dir=0; dir<(dd->nc[dim]==2 ? 1 : 2); dir++) {
      cdd = d*2 + dir;
      /* Communicate the cg and atom counts */
      sbuf[0] = ncg[cdd];
      sbuf[1] = nat[cdd];
      if (debug)
	fprintf(debug,"Sending ddim %d dir %d: ncg %d nat %d\n",
		d,dir,sbuf[0],sbuf[1]);
      dd_sendrecv_int(dd,d,dir,sbuf,2,rbuf,2);

      if ((ncg_recv+rbuf[0])*DD_CGIBS > comm->nalloc_int) {
	comm->nalloc_int = over_alloc((ncg_recv+rbuf[0])*DD_CGIBS);
	srenew(comm->buf_int,comm->nalloc_int);
      }

      /* Communicate the charge group indices, sizes and flags */
      dd_sendrecv_int(dd,d,dir,
		      comm->cggl_flag[cdd],sbuf[0]*DD_CGIBS,
		      comm->buf_int+ncg_recv*DD_CGIBS,rbuf[0]*DD_CGIBS);
      
      nvs = ncg[cdd] + nat[cdd]*nvec;
      i   = rbuf[0]  + rbuf[1] *nvec;
      if (nvr+i > comm->nalloc_vr) {
	comm->nalloc_vr = over_alloc(nvr+i);
	srenew(comm->buf_vr,comm->nalloc_vr);
      }

      /* Communicate cgcm and state */
      dd_sendrecv_rvec(dd,d,dir,
		       comm->cgcm_state[cdd],nvs,
		       comm->buf_vr+nvr,i);
      ncg_recv += rbuf[0];
      nat_recv += rbuf[1];
      nvr      += i;
    }

    /* Process the received charge groups */
    buf_pos = 0;
    for(cg=0; cg<ncg_recv; cg++) {
      flag = comm->buf_int[cg*DD_CGIBS+1];
      mc = -1;
      if (d < dd->ndim-1) {
	/* Check which direction this cg should go */
	for(d2=d+1; (d2<dd->ndim && mc==-1); d2++) {
	  if (dd->bGridJump) {
	    /* The cell boundaries for dimension d2 are not equal
	     * for each cell row of the lower dimension(s),
	     * therefore we might need to redetermine where
	     * this cg should go.
	     */
	    dim2 = dd->dim[d2];
	    /* If this cg crosses the box boundary in dimension d2
	     * we can use the communicated flag, so we do not
	     * have to worry about pbc.
	     */
	    if (!((dd->ci[dim2] == dd->nc[dim2]-1 &&
		   (flag & DD_FLAG_FW(d2))) ||
		  (dd->ci[dim2] == 0 &&
		   (flag & DD_FLAG_BW(d2))))) {
	      /* Clear the two flags for this dimension */
	      flag &= ~(DD_FLAG_FW(d2) | DD_FLAG_BW(d2));
	      /* Determine the location of this cg in lattice coordinates */
	      pos_d = comm->buf_vr[buf_pos][dim2];
	      if (tric_dir[dim2])
		for(d3=dim2+1; d3<DIM; d3++)
		  pos_d -= comm->buf_vr[buf_pos][d3]*state->box[d3][dim2]*invbox[d3];
	      if (pos_d >= cell_x1[dim2]) {
		flag |= DD_FLAG_FW(d2);
	      } else if (pos_d < cell_x0[dim2]) {
		flag |= DD_FLAG_BW(d2);
	      }
	      comm->buf_int[cg*DD_CGIBS+1] = flag;
	    }
	  }
	  /* Set to which neighboring cell this cg should go */
	  if (flag & DD_FLAG_FW(d2)) {
	    mc = d2*2;
	  } else if (flag & DD_FLAG_BW(d2)) {
	    if (dd->nc[dd->dim[d2]] > 2)
	      mc = d2*2+1;
	    else
	      mc = d2*2;
	  }
	}
      }
      
      nrcg = flag & DD_FLAG_NRCG;
      if (mc == -1) {
	if (home_pos_cg+1 > dd->cg_nalloc) {
	  dd->cg_nalloc = over_alloc(home_pos_cg+1);
	  srenew(dd->index_gl,dd->cg_nalloc);
	  srenew(dd->cgindex,dd->cg_nalloc+1);
	}
	/* Set the global charge group index and size */
	dd->index_gl[home_pos_cg] = comm->buf_int[cg*DD_CGIBS];
	dd->cgindex[home_pos_cg+1] = dd->cgindex[home_pos_cg] + nrcg;
	/* Copy the state from the buffer */
	if (home_pos_cg >= fr->cg_nalloc) {
	  dd_realloc_fr_cg(fr,home_pos_cg+1);
	  cg_cm = fr->cg_cm;
	}
	copy_rvec(comm->buf_vr[buf_pos++],cg_cm[home_pos_cg]);
	if (home_pos_at+nrcg > state->nalloc)
	  dd_realloc_state(state,f,buf,home_pos_at+nrcg);
	for(i=0; i<nrcg; i++)
	  copy_rvec(comm->buf_vr[buf_pos++],state->x[home_pos_at+i]);
	if (bV) {
	  for(i=0; i<nrcg; i++)
	    copy_rvec(comm->buf_vr[buf_pos++],state->v[home_pos_at+i]);
	}
	if (bSDX) {
	  for(i=0; i<nrcg; i++)
	    copy_rvec(comm->buf_vr[buf_pos++],state->sd_X[home_pos_at+i]);
	}
	home_pos_cg += 1;
	home_pos_at += nrcg;
      } else {
	/* Reallocate the buffers if necessary  */
	if (ncg[mc]+1 > comm->cggl_flag_nalloc[mc]) {
	  comm->cggl_flag_nalloc[mc] = over_alloc(ncg[mc]+1);
	  srenew(comm->cggl_flag[mc],comm->cggl_flag_nalloc[mc]*DD_CGIBS);
	}
	nvr = ncg[mc] + nat[mc]*nvec;
	if (nvr + 1 + nrcg*nvec > comm->cgcm_state_nalloc[mc]) {
	  comm->cgcm_state_nalloc[mc] = over_alloc(nvr + 1 + nrcg*nvec);
	  srenew(comm->cgcm_state[mc],comm->cgcm_state_nalloc[mc]);
	}
	/* Copy from the receive to the send buffers */
	memcpy(comm->cggl_flag[mc] + ncg[mc]*DD_CGIBS,
	       comm->buf_int + cg*DD_CGIBS,
	       DD_CGIBS*sizeof(int));
	memcpy(comm->cgcm_state[mc][nvr],
	       comm->buf_vr[buf_pos],
	       (1+nrcg*nvec)*sizeof(rvec));
	buf_pos += 1 + nrcg*nvec;
	ncg[mc] += 1;
	nat[mc] += nrcg;
      }
    }
  }

  /* Clear the local indices, except for the home cell.
   * The home cell indices were updated and cleaned in compact_ind.
   */
  clear_dd_indices(dd,dd->nat_home);

  dd->ncg_home = home_pos_cg;
  dd->nat_home = home_pos_at;

  dd->bMasterHasAllCG = FALSE;

  if (debug)
    fprintf(debug,"Finished repartitioning\n");

  return ncg_stay_home;
}

void dd_cycles_add(gmx_domdec_t *dd,float cycles,int ddCycl)
{
  dd->comm->cycl[ddCycl] += cycles;
  dd->comm->cycl_n[ddCycl]++;
}

static double force_flop_count(t_nrnb *nrnb)
{
  int i;
  double sum;

  sum = 0;
  for(i=eNR_NBKERNEL010; i<=eNR_NB14; i++)
    sum += nrnb->n[i]*cost_nrnb(i);
  for(i=eNR_BONDS; i<=eNR_WALLS; i++)
    sum += nrnb->n[i]*cost_nrnb(i);

  return sum;
}

void dd_force_flop_start(gmx_domdec_t *dd,t_nrnb *nrnb)
{
  if (dd->comm->bCountFlop) {
    dd->comm->flop -= force_flop_count(nrnb);
  }
}
void dd_force_flop_stop(gmx_domdec_t *dd,t_nrnb *nrnb)
{
  if (dd->comm->bCountFlop) {
    dd->comm->flop += force_flop_count(nrnb);
    dd->comm->flop_n++;
  }
}  

static void clear_dd_cycle_counts(gmx_domdec_t *dd)
{
  int i;
  
  for(i=0; i<ddCyclNr; i++) {
    dd->comm->cycl[i] = 0;
    dd->comm->cycl_n[i] = 0;
  }
  dd->comm->flop = 0;
  dd->comm->flop_n = 0;
}

static void get_load_distribution(gmx_domdec_t *dd,gmx_wallcycle_t wcycle)
{
#ifdef GMX_MPI
  gmx_domdec_comm_t *comm;
  gmx_domdec_load_t *load;
  int  d,dim,cid,i,pos;
  float cell_frac=0,sbuf[DD_NLOAD_MAX];
  bool bSepPME;

  comm = dd->comm;
  
  bSepPME = (dd->pme_nodeid >= 0);
  
  for(d=dd->ndim-1; d>=0; d--) {
    dim = dd->dim[d];
    /* Check if we participate in the communication in this dimension */
    if (d == dd->ndim-1 || 
	(dd->ci[dd->dim[d+1]]==0 && dd->ci[dd->dim[dd->ndim-1]]==0)) {
      load = &comm->load[d];
      if (dd->bGridJump)
	cell_frac = comm->root[d].cell_f[dd->ci[dim]+1] -
	            comm->root[d].cell_f[dd->ci[dim]  ];
      pos = 0;
      if (d == dd->ndim-1) {
	sbuf[pos++] = dd_force_load(comm);
	sbuf[pos++] = sbuf[0];
	if (dd->bGridJump) {
	  sbuf[pos++] = sbuf[0];
	  sbuf[pos++] = cell_frac;
	  if (d > 0) {
	    sbuf[pos++] = comm->cell_f_max0[d];
	    sbuf[pos++] = comm->cell_f_min1[d];
	  }
	}
	if (bSepPME) {
	  sbuf[pos++] = comm->cycl[ddCyclMoveX] + comm->cycl[ddCyclF] +
	    comm->cycl[ddCyclMoveF];
	  sbuf[pos++] = comm->cycl[ddCyclPME];
	}
      } else {
	sbuf[pos++] = comm->load[d+1].sum;
	sbuf[pos++] = comm->load[d+1].max;
	if (dd->bGridJump) {
	  sbuf[pos++] = comm->load[d+1].sum_m;
	  sbuf[pos++] = comm->load[d+1].cvol_min*cell_frac;
	  sbuf[pos++] = comm->load[d+1].flags;
	  if (d > 0) {
	    sbuf[pos++] = comm->cell_f_max0[d];
	    sbuf[pos++] = comm->cell_f_min1[d];
	  }
	}
	if (bSepPME) {
	  sbuf[pos++] = comm->load[d+1].mdf;
	  sbuf[pos++] = comm->load[d+1].pme;
	}
      }
      load->nload = pos;
      /* Communicate a row in DD direction d.
       * The communicators are setup such that the root always has rank 0.
       */
      MPI_Gather(sbuf      ,load->nload*sizeof(float),MPI_BYTE,
		 load->load,load->nload*sizeof(float),MPI_BYTE,
		 0,comm->mpi_comm_load[d]);
      if (dd->ci[dim] == dd->master_ci[dim]) {
	/* We are the root, process this row */
	load->sum = 0;
	load->max = 0;
	load->sum_m = 0;
	load->cvol_min = 1;
	load->flags = 0;
	load->mdf = 0;
	load->pme = 0;
	pos = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  load->sum += load->load[pos++];
	  load->max = max(load->max,load->load[pos]);
	  pos++;
	  if (dd->bGridJump) {
	    if (comm->root[d].bLimited)
	      /* This direction could not be load balanced properly,
	       * therefore we need to use the maximum iso the average load.
	       */
	      load->sum_m = max(load->sum_m,load->load[pos]);
	    else
	      load->sum_m += load->load[pos];
	    pos++;
	    load->cvol_min = min(load->cvol_min,load->load[pos]);
	    pos++;
	    if (d < dd->ndim-1)
	      load->flags = (int)(load->load[pos++] + 0.5);
	    if (d > 0) {
	      comm->root[d].cell_f_max0[i] = load->load[pos++];
	      comm->root[d].cell_f_min1[i] = load->load[pos++];
	    }
	  }
	  if (bSepPME) {
	    load->mdf = max(load->mdf,load->load[pos]);
	    pos++;
	    load->pme = max(load->pme,load->load[pos]);
	    pos++;
	  }
	}
	if (dd->bDynLoadBal && comm->root[d].bLimited) {
	  load->sum_m *= dd->nc[dim];
	  load->flags |= (1<<d);
	}
      }
    }
  }
#endif
}

static float dd_vol_min(gmx_domdec_t *dd)
{
  return dd->comm->load[0].cvol_min*dd->nnodes;
}

static bool dd_load_flags(gmx_domdec_t *dd)
{
  return dd->comm->load[0].flags;
}

static float dd_f_imbal(gmx_domdec_t *dd)
{
  return dd->comm->load[0].max*dd->nnodes/dd->comm->load[0].sum - 1;
}

static float dd_pme_f_ratio(gmx_domdec_t *dd)
{
  return dd->comm->load[0].pme/dd->comm->load[0].mdf;
}

static void dd_print_load(FILE *fplog,gmx_domdec_t *dd,int step)
{
  int flags,d;

  flags = dd_load_flags(dd);
  if (flags) {
    fprintf(fplog,
	    "DD  load balancing is limited by minimum cell size in dimension");
    for(d=0; d<dd->ndim; d++)
      if (flags & (1<<d))
	fprintf(fplog," %c",dim2char(dd->dim[d]));
    fprintf(fplog,"\n");
  }
  fprintf(fplog,"DD  step %d",step);
  if (dd->bDynLoadBal)
    fprintf(fplog,"  vol min/aver %5.3f%c",dd_vol_min(dd),flags ? '!' : ' ');
  fprintf(fplog," load imb.: force %4.1f%%",dd_f_imbal(dd)*100);
  if (dd->comm->cycl_n[ddCyclPME])
    fprintf(fplog,"  pme mesh/force %5.3f",dd_pme_f_ratio(dd));
  fprintf(fplog,"\n\n");
}

static void dd_print_load_verbose(gmx_domdec_t *dd)
{
  if (dd->bDynLoadBal)
    fprintf(stderr,"vol %4.2f%c ",
	    dd_vol_min(dd),dd_load_flags(dd) ? '!' : ' ');
  fprintf(stderr,"imb F %2d%% ",(int)(dd_f_imbal(dd)*100+0.5));
  if (dd->comm->cycl_n[ddCyclPME])
    fprintf(stderr,"pme/F %4.2f ",dd_pme_f_ratio(dd));
}


#ifdef GMX_MPI
static void make_load_communicator(gmx_domdec_t *dd,MPI_Group g_all,
				   int dim_ind,ivec loc)
{
  MPI_Group g_row;
  MPI_Comm  c_row;
  int  dim,i,*rank;
  ivec loc_c;
  gmx_domdec_root_t *root;

  dim = dd->dim[dim_ind];
  copy_ivec(loc,loc_c);
  snew(rank,dd->nc[dim]);
  for(i=0; i<dd->nc[dim]; i++) {
    loc_c[dim] = i;
    rank[i] = dd_index(dd->nc,loc_c);
  }
  /* Here we create a new group, that does not necessarily
   * include our process. But MPI_Comm_create needs to be
   * called by all the processes in the original communicator.
   * Calling MPI_Group_free afterwards gives errors, so I assume
   * also the group is needed by all processes. (B. Hess)
   */
  MPI_Group_incl(g_all,dd->nc[dim],rank,&g_row);
  MPI_Comm_create(dd->comm->all,g_row,&c_row);
  if (c_row != MPI_COMM_NULL) {
    /* This process is part of the group */
    dd->comm->mpi_comm_load[dim_ind] = c_row;
    if (dd->bGridJump) {
      root = &dd->comm->root[dim_ind];
      snew(root->cell_f,dd->nc[dim]+1+dim_ind*2);
      snew(root->old_cell_f,dd->nc[dim]+1);
      if (dd->ci[dim_ind] == dd->master_ci[dim_ind]) {
	snew(root->cell_size,dd->nc[dim]);
	snew(root->bCellMin,dd->nc[dim]);
	if (dim_ind > 0) {
	  snew(root->cell_f_max0,dd->nc[dim]);
	  snew(root->cell_f_min1,dd->nc[dim]);
	  snew(root->bound_min,dd->nc[dim]);
	  snew(root->bound_max,dd->nc[dim]);
	}
      }
    }
    if (dd->ci[dim_ind] == dd->master_ci[dim_ind])
      snew(dd->comm->load[dim_ind].load,dd->nc[dim]*DD_NLOAD_MAX);
  }
  sfree(rank);
}
#endif

static void make_load_communicators(gmx_domdec_t *dd)
{
#ifdef GMX_MPI
  MPI_Group g_all;
  int  dim0,dim1,i,j;
  ivec loc;

  if (debug)
    fprintf(debug,"Making load communicators\n");

  MPI_Comm_group(dd->comm->all,&g_all);
  
  snew(dd->comm->load,dd->ndim);
  snew(dd->comm->mpi_comm_load,dd->ndim);
  
  clear_ivec(loc);
  make_load_communicator(dd,g_all,0,loc);
  if (dd->ndim > 1) {
    dim0 = dd->dim[0];
    for(i=0; i<dd->nc[dim0]; i++) {
      loc[dim0] = i;
      make_load_communicator(dd,g_all,1,loc);
    }
  }
  if (dd->ndim > 2) {
    dim0 = dd->dim[0];
    for(i=0; i<dd->nc[dim0]; i++) {
      loc[dim0] = i;
      dim1 = dd->dim[1];
      for(j=0; j<dd->nc[dim1]; j++) {
	  loc[dim1] = j;
	  make_load_communicator(dd,g_all,2,loc);
      }
    }
  }

  MPI_Group_free(&g_all);

  if (debug)
    fprintf(debug,"Finished making load communicators\n");
#endif
}

void setup_dd_grid(FILE *fplog,gmx_domdec_t *dd)
{
  bool bZYX;
  int  start,inc;
  int  d,i,j,m;
  ivec tmp,s;
  int  ncell,ncellp;
  ivec dd_cp[DD_MAXICELL];
  gmx_domdec_ns_ranges_t *icell;

  if (getenv("GMX_DD_ORDER_ZYX")) {
    /* Decomposition order z,y,x */
    fprintf(fplog,"Using domain decomposition order z, y, x\n");
    start = DIM-1;
    inc   = -1;
  } else {
    /* Decomposition order x,y,z */
    start = 0;
    inc   = 1;
  }

  /* The x/y/z communication order is set by this loop */
  dd->ndim = 0;
  for(d=start; (d>=0 && d<DIM); d+=inc) {
    if (dd->nc[d] > 1) {
      dd->dim[dd->ndim] = d;
      copy_ivec(dd->ci,tmp);
      tmp[d] = (tmp[d] + 1) % dd->nc[d];
      dd->neighbor[dd->ndim][0] = ddcoord2ddnodeid(dd,tmp);
      copy_ivec(dd->ci,tmp);
      tmp[d] = (tmp[d] - 1 + dd->nc[d]) % dd->nc[d];
      dd->neighbor[dd->ndim][1] = ddcoord2ddnodeid(dd,tmp);
      if (debug)
	fprintf(debug,"DD rank %d neighbor ranks in dir %d are + %d - %d\n",
		dd->rank,d,
		dd->neighbor[dd->ndim][0],
		dd->neighbor[dd->ndim][1]);
      dd->ndim++;
    }
  }
  
  fprintf(fplog,"Making %dD domain decomposition %d x %d x %d, home cell index %d %d %d\n",
	  dd->ndim,
	  dd->nc[XX],dd->nc[YY],dd->nc[ZZ],
	  dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  if (DDMASTER(dd))
    fprintf(stderr,"Making %dD domain decomposition %d x %d x %d\n",
	    dd->ndim,dd->nc[XX],dd->nc[YY],dd->nc[ZZ]);
  switch (dd->ndim) {
  case 3:
    ncell  = dd_c3n;
    ncellp = dd_cp3n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp3[i],dd_cp[i]);
    break;
  case 2:
    ncell  = dd_c2n;
    ncellp = dd_cp2n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp2[i],dd_cp[i]);
    break;
  case 1:
    ncell  = dd_c1n;
    ncellp = dd_cp1n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp1[i],dd_cp[i]);
    break;
  default:
    gmx_fatal(FARGS,"Can only do 1, 2 or 3D domain decomposition");
    ncell = 0;
    ncellp = 0;
  }
    
  for(i=0; i<ncell; i++) {
    m = 0;
    for(d=start; (d>=0 && d<DIM); d+=inc) {
      if (dd->nc[d] > 1)
	dd->shift[i][d] = dd_co[i][m++];
      else 
	dd->shift[i][d] = 0;
    }
  }

  dd->ncell  = ncell;
  for(i=0; i<ncell; i++) {
    for(d=0; d<DIM; d++) {
      s[d] = dd->ci[d] - dd->shift[i][d];
      if (s[d] < 0)
	s[d] += dd->nc[d];
      else if (s[d] >= dd->nc[d])
	s[d] -= dd->nc[d];
    }
  }
  dd->nicell = ncellp;
  for(i=0; i<dd->nicell; i++) {
    if (dd_cp[i][0] != i)
      gmx_fatal(FARGS,"Internal inconsistency in the dd grid setup");
    icell = &dd->icell[i];
    icell->j0 = dd_cp[i][1];
    icell->j1 = dd_cp[i][2];
    for(d=0; d<DIM; d++) {
      if (dd->nc[d] == 1) {
	/* All shifts should be allowed */
	icell->shift0[d] = -1;
	icell->shift1[d] = 1;
      } else {
	/*
	icell->shift0[d] = 0;
	icell->shift1[d] = 0;
	for(j=icell->j0; j<icell->j1; j++) {
	  if (dd->shift[j][d] > dd->shift[i][d])
	    icell->shift0[d] = -1;
	  if (dd->shift[j][d] < dd->shift[i][d])
	    icell->shift1[d] = 1;
	}
	*/
	
	int shift_diff;
	
	/* Assume the shift are not more than 1 cell */
	icell->shift0[d] = 1;
	icell->shift1[d] = -1;
	for(j=icell->j0; j<icell->j1; j++) {
	  shift_diff = dd->shift[j][d] - dd->shift[i][d];
	  if (shift_diff < icell->shift0[d])
	    icell->shift0[d] = shift_diff;
	  if (shift_diff > icell->shift1[d])
	    icell->shift1[d] = shift_diff;
	}
      }
    }
  }

  if (dd->bGridJump)
    snew(dd->comm->root,dd->ndim);

  if (dd->comm->bRecordLoad)
    make_load_communicators(dd);
}

static void dd_cart_coord2pmecoord(gmx_domdec_t *dd,ivec coord,ivec coord_pme)
{
  int nc,ntot;

  nc   = dd->nc[dd->comm->pmedim];
  ntot = dd->comm->ntot[dd->comm->pmedim];
  copy_ivec(coord,coord_pme);
  coord_pme[dd->comm->pmedim] =
    nc + (coord[dd->comm->pmedim]*(ntot - nc) + (ntot - nc)/2)/nc;
}

static int ddindex2pmeslab(const t_commrec *cr,int ddindex)
{
  /* We add cr->npmenodes/2 to obtain an even distribution */
  return (ddindex*cr->npmenodes + cr->npmenodes/2)/cr->dd->nnodes;
}

static int *dd_pmenodes(t_commrec *cr)
{
  int *pmenodes;
  int n,i,p0,p1;

  snew(pmenodes,cr->npmenodes);
  n = 0;
  for(i=0; i<cr->dd->nnodes; i++) {
    p0 = ddindex2pmeslab(cr,i);
    p1 = ddindex2pmeslab(cr,i+1);
    if (i+1 == cr->dd->nnodes || p1 > p0) {
      if (debug)
	fprintf(debug,"pmenode[%d] = %d\n",n,i+1+n);
      pmenodes[n] = i + 1 + n;
      n++;
    }
  }

  return pmenodes;
}

int gmx_ddcoord2pmeslab(t_commrec *cr,int x,int y,int z)
{
  gmx_domdec_t *dd;
  ivec coords,coords_pme,nc;
  int  slab;

  dd = cr->dd;
  /*
  if (dd->comm->bCartesian) {
    gmx_ddindex2xyz(dd->nc,ddindex,coords);
    dd_coords2pmecoords(dd,coords,coords_pme);
    copy_ivec(dd->ntot,nc);
    nc[dd->pmedim]         -= dd->nc[dd->pmedim];
    coords_pme[dd->pmedim] -= dd->nc[dd->pmedim];

    slab = (coords_pme[XX]*nc[YY] + coords_pme[YY])*nc[ZZ] + coords_pme[ZZ];
  } else {
    slab = (ddindex*cr->npmenodes + cr->npmenodes/2)/dd->nnodes;
  }
  */
  coords[XX] = x;
  coords[YY] = y;
  coords[ZZ] = z;
  slab = ddindex2pmeslab(cr,dd_index(cr->dd->nc,coords));

  return slab;
}

static int ddcoord2simnodeid(t_commrec *cr,int x,int y,int z)
{
  gmx_domdec_comm_t *comm;
  ivec coords;
  int  ddindex,nodeid=-1;

  comm = cr->dd->comm;

  coords[XX] = x;
  coords[YY] = y;
  coords[ZZ] = z;
  if (comm->bCartesianPP_PME) {
#ifdef GMX_MPI
    MPI_Cart_rank(cr->mpi_comm_mysim,coords,&nodeid);
#endif
  } else {
    ddindex = dd_index(cr->dd->nc,coords);
    if (comm->bCartesianPP) {
      nodeid = comm->ddindex2simnodeid[ddindex];
    } else {
      if (comm->pmenodes) {
	nodeid = ddindex + gmx_ddcoord2pmeslab(cr,x,y,z);
      } else {
	nodeid = ddindex;
      }
    }
  }
  
  return nodeid;
}

static int dd_node2pmenode(t_commrec *cr,int nodeid)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  ivec coord,coord_pme;
  int  i;
  int  pmenode=-1;
 
  dd = cr->dd;
  comm = dd->comm;

  /* This assumes a uniform x domain decomposition grid cell size */
  if (comm->bCartesianPP_PME) {
#ifdef GMX_MPI
    MPI_Cart_coords(cr->mpi_comm_mysim,nodeid,DIM,coord);
    if (coord[comm->pmedim] < dd->nc[comm->pmedim]) {
      /* This is a PP node */
      dd_cart_coord2pmecoord(dd,coord,coord_pme);
      MPI_Cart_rank(cr->mpi_comm_mysim,coord_pme,&pmenode);
    }
#endif
  } else if (comm->bCartesianPP) {
    if (nodeid < dd->nnodes) {
      pmenode = dd->nnodes + ddindex2pmeslab(cr,nodeid);
    }
  } else {
    /* This assumes DD cells with identical x coordinates
     * are numbered sequentially.
     */
    if (dd->comm->pmenodes == NULL) {
      if (nodeid < dd->nnodes) {
	/* The DD index equals the nodeid */
	pmenode = dd->nnodes + ddindex2pmeslab(cr,nodeid);
      }
    } else {
      i = 0;
      while (nodeid > dd->comm->pmenodes[i])
	i++;
      if (nodeid < dd->comm->pmenodes[i])
	pmenode = dd->comm->pmenodes[i];
    }
  }

  return pmenode;
}

bool gmx_pmeonlynode(t_commrec *cr,int nodeid)
{
  bool bPMEOnlyNode;

  if (DOMAINDECOMP(cr)) {
    bPMEOnlyNode = (dd_node2pmenode(cr,nodeid) == -1);
  } else {
    bPMEOnlyNode = FALSE;
  }

  return bPMEOnlyNode;
}

void get_pme_ddnodes(FILE *logfile,t_commrec *cr,int pmenodeid,
		     int *nmy_ddnodes,int **my_ddnodes)
{
  gmx_domdec_t *dd;
  int x,y,z;
  ivec coord,coord_pme;

  dd = cr->dd;

  snew(*my_ddnodes,(dd->nnodes+cr->npmenodes-1)/cr->npmenodes);
  
  *nmy_ddnodes = 0;
  for(x=0; x<dd->nc[XX]; x++) {
    for(y=0; y<dd->nc[YY]; y++) {
      for(z=0; z<dd->nc[ZZ]; z++) {
	if (dd->comm->bCartesianPP_PME) {
	  coord[XX] = x;
	  coord[YY] = y;
	  coord[ZZ] = z;
	  dd_cart_coord2pmecoord(dd,coord,coord_pme);
	  if (dd->ci[XX] == coord_pme[XX] &&
	      dd->ci[YY] == coord_pme[YY] &&
	      dd->ci[ZZ] == coord_pme[ZZ])
	    (*my_ddnodes)[(*nmy_ddnodes)++] = ddcoord2simnodeid(cr,x,y,z);
	} else {
	  /* The slab corresponds to the nodeid in the PME group */
	  if (gmx_ddcoord2pmeslab(cr,x,y,z) == pmenodeid)
	    (*my_ddnodes)[(*nmy_ddnodes)++] = ddcoord2simnodeid(cr,x,y,z);
	}
      }
    }
  }

  fprintf(logfile,"PME node %d, receive coordinates from %d PP nodes\n",
	  cr->nodeid,*nmy_ddnodes);
  if (debug) {
    fprintf(debug,"Receive coordinates from PP nodes:");
    for(x=0; x<*nmy_ddnodes; x++)
      fprintf(debug," %d",(*my_ddnodes)[x]);
    fprintf(debug,"\n");
  }
}

static bool receive_vir_ener(t_commrec *cr)
{
  gmx_domdec_comm_t *comm;
  int  pmenode,coords[DIM],rank;
  bool bReceive;

  pmenode = dd_node2pmenode(cr,cr->nodeid);

  bReceive = TRUE;
  if (cr->npmenodes < cr->dd->nnodes) {
    comm = cr->dd->comm;
    if (comm->bCartesianPP_PME) {
#ifdef GMX_MPI
      MPI_Cart_coords(cr->mpi_comm_mysim,cr->nodeid,DIM,coords);
      coords[comm->pmedim]++;
      if (coords[comm->pmedim] < cr->dd->nc[comm->pmedim]) {
	MPI_Cart_rank(cr->mpi_comm_mysim,coords,&rank);
	if (dd_node2pmenode(cr,rank) == pmenode) {
	  /* This is not the last PP node for pmenode */
	  bReceive = FALSE;
	}
      }
#endif  
    } else {
      if (cr->nodeid+1 < cr->nnodes &&
	  dd_node2pmenode(cr,cr->nodeid+1) == pmenode) {
	/* This is not the last PP node for pmenode */
	bReceive = FALSE;
      }
    }
  }
  
  return bReceive;
}

void make_dd_communicators(FILE *fplog,t_commrec *cr,
			   bool bCartesian,bool bOrderPP_PME)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  bool bDiv[DIM];
  int  i,rank,*buf;
  ivec periods;
#ifdef GMX_MPI
  MPI_Comm comm_cart;
#endif

  dd = cr->dd;
  comm = dd->comm;

  dd->sim_nodeid = cr->nodeid;

  copy_ivec(dd->nc,comm->ntot);
  
  comm->bCartesianPP = bCartesian;
  comm->bCartesianPP_PME = FALSE;
  if (comm->bCartesianPP && cr->npmenodes > 0) {
    for(i=1; i<DIM; i++)
      bDiv[i] = ((cr->npmenodes*dd->nc[i]) % (dd->nnodes) == 0);
    if (bDiv[YY] || bDiv[ZZ]) {
      comm->bCartesianPP_PME = TRUE;
      /* We choose the direction that provides the thinnest slab
       * of PME only nodes as this will have the least effect
       * on the PP communication.
       * But for the PME communication the opposite might be better.
       */
      if (bDiv[YY] && (!bDiv[ZZ] || dd->nc[YY] <= dd->nc[ZZ])) {
	comm->pmedim = YY;
      } else {
	comm->pmedim = ZZ;
      }
      comm->ntot[comm->pmedim]
	+= (cr->npmenodes*dd->nc[comm->pmedim])/dd->nnodes;
    } else {
      fprintf(fplog,"#pmenodes (%d) is not a multiple of nx*ny (%d*%d) or nx*nz (%d*%d)\n",cr->npmenodes,dd->nc[XX],dd->nc[YY],dd->nc[XX],dd->nc[ZZ]);
      fprintf(fplog,
	      "Will not use a Cartesian communicator for PP <-> PME\n\n");
    }
  }

#ifdef GMX_MPI
  if (comm->bCartesianPP_PME) {
    fprintf(fplog,
	    "Will use a Cartesian communicator for PP <-> PME: %d x %d x %d\n",
	    comm->ntot[XX],comm->ntot[YY],comm->ntot[ZZ]);

    for(i=0; i<DIM; i++)
      periods[i] = TRUE;
    MPI_Cart_create(cr->mpi_comm_mysim,DIM,comm->ntot,periods,TRUE,&comm_cart);

    MPI_Comm_rank(comm_cart,&rank);
    if (MASTERNODE(cr) && rank != 0)
      gmx_fatal(FARGS,"MPI rank 0 was renumbered by MPI_Cart_create, we do not allow this");

    /* With this assigment we loose the link to the original communicator
     * which will usually be MPI_COMM_WORLD, unless have multisim.
     */
    cr->mpi_comm_mysim = comm_cart;
    cr->nodeid = rank;

    MPI_Cart_coords(cr->mpi_comm_mysim,cr->nodeid,DIM,dd->ci);

    fprintf(fplog,"Cartesian nodeid %d, coordinates %d %d %d\n\n",
	    cr->nodeid,dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
    
    if (dd->ci[comm->pmedim] < dd->nc[comm->pmedim])
      cr->duty |= DUTY_PP;
    if (cr->npmenodes == 0 || dd->ci[comm->pmedim] >= dd->nc[comm->pmedim])
      cr->duty |= DUTY_PME;

    /* Split the sim communicator into PP and PME only nodes */
    MPI_Comm_split(cr->mpi_comm_mysim,
		   cr->duty,
		   dd_index(comm->ntot,dd->ci),
		   &cr->mpi_comm_mygroup);
  } else {
    if (cr->npmenodes == 0) {
      cr->duty |= (DUTY_PP | DUTY_PME);

      cr->mpi_comm_mygroup = cr->mpi_comm_mysim;
    } else {
      if (comm->bCartesianPP || bOrderPP_PME) {
	fprintf(fplog,"Order of the nodes: PP first, PME last\n");
      } else {
	/* Interleave the PP-only and PME-only nodes,
	 * as on clusters with dual-core machines this will double
	 * the communication bandwidth of the PME processes
	 * and thus speed up the PP <-> PME and inter PME communication.
	 */
	fprintf(fplog,"Interleaving PP and PME nodes\n");
	comm->pmenodes = dd_pmenodes(cr);
      }

      if (dd_node2pmenode(cr,cr->nodeid) == -1)
	cr->duty |= DUTY_PME;
      else
	cr->duty |= DUTY_PP;

      /* Split the sim communicator into PP and PME only nodes */
      MPI_Comm_split(cr->mpi_comm_mysim,
		     cr->duty,
		     cr->nodeid,
		     &cr->mpi_comm_mygroup);
    }
  }
  
  if (cr->duty & DUTY_PP) {
    if (comm->bCartesianPP) {
      /* Set up cartesian communication for the particle-particle part */
      fprintf(fplog,"Will use a Cartesian communicator: %d x %d x %d\n",
	      dd->nc[XX],dd->nc[YY],dd->nc[ZZ]);
      
      for(i=0; i<DIM; i++)
	periods[i] = TRUE;
      MPI_Cart_create(cr->mpi_comm_mygroup,DIM,dd->nc,periods,TRUE,&comm_cart);
      /* We overwrite the old communicator with the new cartesian one */
      cr->mpi_comm_mygroup = comm_cart;
    }
    
    comm->all = cr->mpi_comm_mygroup;
    MPI_Comm_rank(comm->all,&dd->rank);
    
    if (comm->bCartesianPP && !comm->bCartesianPP_PME)
      MPI_Cart_coords(comm->all,dd->rank,DIM,dd->ci);
  }
   
  if (comm->bCartesianPP) {
    /* We need to make an index to go from the coordinates
     * to the nodeid of this simulation.
     */
    snew(comm->ddindex2simnodeid,dd->nnodes);
    snew(buf,dd->nnodes);
    if (cr->duty & DUTY_PP)
      buf[dd_index(dd->nc,dd->ci)] = dd->sim_nodeid;
    /* Communicate the ddindex to simulation nodeid index */
    MPI_Allreduce(buf,comm->ddindex2simnodeid,dd->nnodes,MPI_INT,MPI_SUM,
		  cr->mpi_comm_mysim);
    sfree(buf);
  }
  
  if (cr->duty & DUTY_PP) {
    if (comm->bCartesianPP) {
      /* Determine the master coordinates and rank.
       * The DD master should be the same node as the master of this sim.
       */
      dd->masterrank = -1;
      for(i=0; i<dd->nnodes; i++) {
	if (comm->ddindex2simnodeid[i] == 0) {
	  ddindex2xyz(dd->nc,i,dd->master_ci);
	  MPI_Cart_rank(comm->all,dd->master_ci,&dd->masterrank);
	}
      }
      if (dd->masterrank == -1)
	gmx_fatal(FARGS,"MPI_Cart_create has remapped the cells such that the orginal master node has become a PME node, can not continue");
          
      if (comm->bCartesianPP_PME) {
	/* Since we want to use the original cartesian setup for sim,
	 * and not the one after split, we need to make an index.
	 */
	snew(comm->ddindex2ddnodeid,dd->nnodes);
	comm->ddindex2ddnodeid[dd_index(dd->nc,dd->ci)] = dd->rank;
	gmx_sumi(dd->nnodes,comm->ddindex2ddnodeid,cr);
      }
    } else { 
      /* We use the rank in dd->comm->all as DD index */
      ddindex2xyz(dd->nc,dd->rank,dd->ci);
      /* The simulation master nodeid is 0, so the DD master rank is also 0 */
      dd->masterrank = 0;
      for(i=0; i<DIM; i++)
	dd->master_ci[i] = 0;
    }
    
    fprintf(fplog,"Domain decomposition nodeid %d, coordinates %d %d %d\n\n",
	    dd->rank,dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  }
  
  if (cr->npmenodes > 0) {
    if (cr->duty & DUTY_PP) {
      /* Make the ring smaller */
      cr->left  = (dd->rank - 1 + dd->nnodes) % dd->nnodes;
      cr->right = (dd->rank + 1) % dd->nnodes;
    }

    fprintf(fplog,"This is a %s only node\n\n",
	    (cr->duty & DUTY_PP) ? "particle-particle" : "PME-mesh");
  }
#endif
  
  if (!(cr->duty & DUTY_PME)) {
    dd->pme_nodeid = dd_node2pmenode(cr,cr->nodeid);
    dd->pme_receive_vir_ener = receive_vir_ener(cr);
    if (debug)
      fprintf(debug,"My pme_nodeid %d receive ener %d\n",
	      dd->pme_nodeid,dd->pme_receive_vir_ener);
  } else {
    dd->pme_nodeid = -1;
  }

  if (DDMASTER(dd) && dd->ma == NULL) {
    snew(dd->ma,1);
  }
}

static real *get_cell_load(FILE *fplog,char *dir,int nc,char *load_string)
{
  real *cell_load;
  int  i,n;
  double dbl;

  cell_load = NULL;
  if (nc > 1 && load_string != NULL) {
    fprintf(fplog,"Using static load balancing for the %s direction\n",dir);
    snew(cell_load,nc);
    for (i=0; i<nc; i++) {
      dbl = 0;
      sscanf(load_string,"%lf%n",&dbl,&n);
      if (dbl == 0)
	gmx_fatal(FARGS,
		  "Incorrect or not enough load entries for direction %s",
		  dir);
      cell_load[i] = dbl;
      load_string += n;
    }
  }
  
  return cell_load;
}

static int dd_nst_env(FILE *fplog,char *env_var)
{
  char *val;
  int  nst;

  nst = 0;
  val = getenv(env_var);
  if (val) {
    if (sscanf(val,"%d",&nst) <= 0)
      nst = 1;
    fprintf(fplog,"Found env.var. %s = %s, using value %d\n",
	    env_var,val,nst);
  }
  
  return nst;
}

gmx_domdec_t *init_domain_decomposition(FILE *fplog,t_commrec *cr,ivec nc,
					real comm_distance_min,
					bool bDynLoadBal,
					char *loadx,char *loady,char *loadz)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  int  d,i,j;
  char *warn="WARNING: Cycle counting is not supported on this architecture, will not use dynamic load balancing";

  fprintf(fplog,
	  "Domain decomposition grid %d x %d x %d, separate PME nodes %d\n"
	  "Total number of nodes is %d\n",
	  nc[XX],nc[YY],nc[ZZ],cr->npmenodes,cr->nnodes);

  snew(dd,1);
  snew(dd->comm,1);
  comm = dd->comm;
  snew(comm->cggl_flag,DIM*2);
  snew(comm->cgcm_state,DIM*2);

  copy_ivec(nc,dd->nc);
  dd->nnodes = dd->nc[XX]*dd->nc[YY]*dd->nc[ZZ];
  if (dd->nnodes != cr->nnodes - cr->npmenodes)
    gmx_fatal(FARGS,"The size of the domain decomposition grid (%d) does not match the number of nodes (%d). Total number of nodes is %d\n",
	      dd->nnodes,cr->nnodes - cr->npmenodes,cr->nnodes);
  dd->ndim = 0;
  for(d=0; d<DIM; d++)
    if (dd->nc[d] > 1)
      dd->ndim++;

  comm->distance_min = comm_distance_min;

  comm->bSendRecv2 = dd_nst_env(fplog,"GMX_DD_SENDRECV2");
  comm->bCountFlop = dd_nst_env(fplog,"GMX_DLB_FLOP");
  nstDDDump        = dd_nst_env(fplog,"GMX_DD_DUMP");
  nstDDDumpGrid    = dd_nst_env(fplog,"GMX_DD_DUMP_GRID");
  if (comm->bSendRecv2)
    fprintf(fplog,"Will use two sequential MPI_Sendrecv calls instead of two simultaneous non-blocking MPI_Irecv and MPI_Isend pairs for constraint and vsite communication\n");
  if (comm->bCountFlop) {
    fprintf(fplog,"Will load balance based on FLOP count\n");
    comm->bRecordLoad = TRUE;
  } else {
    comm->bRecordLoad = wallcycle_have_counter();
  }

  dd->bDynLoadBal = FALSE;
  if (bDynLoadBal) {
    if (comm->bRecordLoad) {
      dd->bDynLoadBal = TRUE;
    } else {
      fprintf(fplog,"\n%s\n\n",warn);
      fprintf(stderr,"\n%s\n\n",warn);
    }
  }
  fprintf(fplog,"Will%s use dynamic load balancing\n",
	  dd->bDynLoadBal ? "" : " not");
  dd->bGridJump = dd->bDynLoadBal;
  snew(dd->cell_load,DIM);
  if (comm->bRecordLoad) {
    if (dd->ndim > 1) {
      snew(comm->cell_d1,2);
      for(i=0; i<2; i++)
	snew(comm->cell_d1[i],2);
    }
    if (dd->ndim > 2) {
      snew(comm->cell_d2,2);
      for(i=0; i<2; i++) {
	snew(comm->cell_d2[i],2);
	for(j=0; j<2; j++)
	  snew(comm->cell_d2[i][j],2);
      }
    }
  }
  if (!dd->bDynLoadBal) {
    dd->cell_load[XX] = get_cell_load(fplog,"x",dd->nc[XX],loadx);
    dd->cell_load[YY] = get_cell_load(fplog,"y",dd->nc[YY],loady);
    dd->cell_load[ZZ] = get_cell_load(fplog,"z",dd->nc[ZZ],loadz);
  }

  return dd;
}

void set_dd_parameters(FILE *fplog,gmx_domdec_t *dd,
		       t_topology *top,t_inputrec *ir,t_forcerec *fr)
{
  gmx_domdec_comm_t *comm;

  if (dd->pme_nodeid >= 0 && !EEL_PME(ir->coulombtype))
    gmx_fatal(FARGS,
	      "Can not have separate PME nodes without PME electrostatics");

  if (ir->ePBC == epbcNONE)
    gmx_fatal(FARGS,"pbc type %s is not supported with domain decomposition",
	      epbc_names[epbcNONE]);
  if (ir->ePBC!=epbcXYZ && ir->nwall<2 && dd->nc[ZZ]>1)
    gmx_fatal(FARGS,"Can not do domain decomposition in the z-direction with pbc=%s,",epbc_names[ir->ePBC]);

  if (dd->nc[XX]>1 && dd->nc[YY]>1 && (dd->nc[ZZ]>1 || fr->ePBC==epbcXY))
    fr->bMolPBC = FALSE;
  else
    fr->bMolPBC = TRUE;

  if (ir->ns_type == ensSIMPLE)
    gmx_fatal(FARGS,"ns type %s is not supported with domain decomposition",
	      ens_names[ensSIMPLE]);
  
  if (ir->eConstrAlg == estSHAKE)
    gmx_fatal(FARGS,
	      "%s is not supported (yet) with domain decomposition, use %s",
	      eshake_names[estSHAKE],eshake_names[estLINCS]);

  fr->solvent_type_global = fr->solvent_type;
  fr->solvent_type        = NULL;

  comm = dd->comm;

  comm->bInterCGBondeds = (top->blocks[ebCGS].nr > top->blocks[ebMOLS].nr);

  comm->distance = fr->rlistlong;
  if (comm->bInterCGBondeds) {
    comm->distance = max(comm->distance,comm->distance_min);
    if (DDMASTER(dd))
      fprintf(fplog,"\nAtoms involved in bonded interactions should be within %g nm\n",comm->distance);
  }
}

static void setup_dd_communication(FILE *fplog,int step,
				   gmx_domdec_t *dd,t_block *gcgs,
				   matrix box,t_forcerec *fr)
{
  int dim_ind,dim,dim0,dim1=-1,nat_tot,ncell,cell,celli,c,i,j,cg,cg_gl,nrcg;
  int *ncg_cell,*index_gl,*cgindex;
  gmx_domdec_comm_t *comm;
  gmx_domdec_ind_t *ind;
  real r_comm2,r,r2,inv_ncg;
  real corner[DIM][4],corner_round_0=0,corner_round_1[4];
  ivec tric_dist;
  rvec *cg_cm,*v_d,*v_0=NULL,*v_1=NULL;
  real skew_fac2_d,skew_fac2_0=0,skew_fac2_1=0;
  int  nsend,nat;

  if (debug)
    fprintf(debug,"Setting up DD communication\n");

  comm  = dd->comm;
  cg_cm = fr->cg_cm;

  for(dim_ind=0; dim_ind<dd->ndim; dim_ind++) {
    dim = dd->dim[dim_ind];
    
    if ((dd->cell_x1[dim] - dd->cell_x0[dim])*dd->skew_fac[dim] <
	comm->distance)
      gmx_fatal(FARGS,"Step %d: The %c-size (%f) times the triclinic skew factor (%f)is smaller than the cut-off (%f) for domain decomposition grid cell %d %d %d (node %d)",
		step,dim2char(dim),
		dd->cell_x1[dim] - dd->cell_x0[dim],dd->skew_fac[dim],
		comm->distance,
		dd->ci[XX],dd->ci[YY],dd->ci[ZZ],dd->sim_nodeid);
    
    /* Check if we need to use triclinic distances */
    tric_dist[dim_ind] = 0;
    for(i=0; i<=dim_ind; i++)
      if (dd->tric_dir[dd->dim[i]])
	tric_dist[dim_ind] = 1;
  }

  /* Set the size of the ns grid,
   * for dynamic load balancing this is corrected in dd_move_cellx.
   */
  copy_rvec(dd->cell_x0,dd->cell_ns_x0);
  copy_rvec(dd->cell_x1,dd->cell_ns_x1);
  
  if (dd->bGridJump && dd->ndim > 1) {
    dd_move_cellx(dd,box);
    check_grid_jump(step,dd,box);
  }

  dim0 = dd->dim[0];
  /* The first dimension is equal for all cells */
  corner[0][0] = dd->cell_x0[dim0];
  if (dd->ndim >= 2) {
    dim1 = dd->dim[1];
    /* This cell row is only seen from the first row */
    corner[1][0] = dd->cell_x0[dim1];
    /* All rows can see this row */
    corner[1][1] = dd->cell_x0[dim1];
    if (dd->bGridJump) {
      corner[1][1] = max(dd->cell_x0[dim1],comm->cell_d1[1][0]);
      if (comm->bInterCGBondeds) {
	/* For the bonded distance we need the maximum */
	corner[1][0] = corner[1][1];
      }
    }
    /* Set the upper-right corner for rounding */
    corner_round_0 = dd->cell_x1[dim0];

    if (dd->ndim >= 3) {
      for(j=0; j<4; j++)
	corner[2][j] = dd->cell_x0[dd->dim[2]];
      if (dd->bGridJump) {
	/* Use the maximum of the i-cells that see a j-cell */
	for(i=0; i<dd->nicell; i++) {
	  for(j=dd->icell[i].j0; j<dd->icell[i].j1; j++) {
	    if (j >= 4) {
	      corner[2][j-4] =
		max(corner[2][j-4],
		    comm->cell_d2[dd->shift[i][dim0]][dd->shift[i][dim1]][0]);
	    }
	  }
	}
	if (comm->bInterCGBondeds) {
	  /* For the bonded distance we need the maximum */
	  for(j=0; j<4; j++)
	    corner[2][j] = corner[2][1];
	}
      }
      
      /* Set the upper-right corner for rounding */
      /* Cell (0,0,0) and cell (1,0,0) can see cell 4 (0,1,1)
       * Only cell (0,0,0) can see cell 7 (1,1,1)
       */
      corner_round_1[0] = dd->cell_x1[dim1];
      corner_round_1[3] = dd->cell_x1[dim1];
      if (dd->bGridJump) {
	corner_round_1[0] = max(dd->cell_x1[dim1],comm->cell_d1[1][1]);
	if (comm->bInterCGBondeds) {
	  /* For the bonded distance we need the maximum */
	  corner_round_1[3] = corner_round_1[0];
	}
      }
    }
  }

  r_comm2 = sqr(comm->distance);

  /* Triclinic stuff */
  if (dd->ndim >= 2) {
    v_0 = comm->v[dim0];
    skew_fac2_0 = sqr(dd->skew_fac[dim0]);
  }
  if (dd->ndim >= 3) {
    v_1 = comm->v[dim1];
    skew_fac2_1 = sqr(dd->skew_fac[dim1]);
  }

  ncg_cell = dd->ncg_cell;
  index_gl = dd->index_gl;
  cgindex  = dd->cgindex;
  
  ncg_cell[0] = 0;
  ncg_cell[1] = dd->ncg_home;

  nat_tot = dd->nat_home;
  ncell = 1;
  for(dim_ind=0; dim_ind<dd->ndim; dim_ind++) {
    dim = dd->dim[dim_ind];

    v_d = comm->v[dim];
    skew_fac2_d = sqr(dd->skew_fac[dim]);

    ind = &comm->ind[dim_ind];
    nsend = 0;
    nat = 0;
    for(cell=0; cell<ncell; cell++) {
      ind->nsend[cell] = 0;
      celli = cell_perm[dim_ind][cell];
      for(cg=ncg_cell[celli]; cg<ncg_cell[celli+1]; cg++) {
	r2 = 0;
	if (tric_dist[dim_ind] == 0) {
	  /* Rectangular box, easy */
	  r = cg_cm[cg][dim] - corner[dim_ind][cell];
	  if (r > 0)
	    r2 += r*r;
	  /* Rounding gives at most a 16% reduction in communicated atoms */
	  if (dim_ind >= 1 && (celli == 1 || celli == 2)) {
	    r = cg_cm[cg][dim0] - corner_round_0;
	    r2 += r*r;
	  }
	  if (dim_ind == 2 && (celli == 2 || celli == 3)) {
	    r = cg_cm[cg][dim1] - corner_round_1[cell];
	    if (r > 0)
	      r2 += r*r;
	  }
	} else {
	  /* Triclinic box, complicated */
	  r = cg_cm[cg][dim] - corner[dim_ind][cell];
	  for(i=dim+1; i<DIM; i++)
	    r -= cg_cm[cg][i]*v_d[i][dim];
	  if (r > 0)
	    r2 += r*r*skew_fac2_d;
	  /* Rounding, conservative as the skew_fac multiplication
	   * will slightly underestimate the distance.
	   */
	  if (dim_ind >= 1 && (celli == 1 || celli == 2)) {
	    r = cg_cm[cg][dim0] - corner_round_0;
	    for(i=dim0+1; i<DIM; i++)
	      r -= cg_cm[cg][i]*v_0[i][dim0];
	    r2 += r*r*skew_fac2_0;
	  }
	  if (dim_ind == 2 && (celli == 2 || celli == 3)) {
	    r = cg_cm[cg][dim1] - corner_round_1[cell];
	    for(i=dim1+1; i<DIM; i++)
	      r -= cg_cm[cg][i]*v_1[i][dim1];
	    if (r > 0)
	      r2 += r*r*skew_fac2_1;
	  }
	}
	if (r2 < r_comm2) {
	  /* Make an index to the local charge groups */
	  if (nsend >= ind->nalloc) {
	    ind->nalloc += CG_ALLOC_SIZE;
	    srenew(ind->index,ind->nalloc);
	  }
	  if (nsend >= comm->nalloc_int) {
	    comm->nalloc_int += CG_ALLOC_SIZE;
	    srenew(comm->buf_int,comm->nalloc_int);
	  }
	  ind->index[nsend] = cg;
	  comm->buf_int[nsend] = index_gl[cg];
	  ind->nsend[cell]++;
	  if (nsend >= comm->nalloc_vr) {
	    comm->nalloc_vr += CG_ALLOC_SIZE;
	    srenew(comm->buf_vr,comm->nalloc_vr);
	  }
	  if (dd->ci[dim] == 0) {
	    /* Correct cg_cm for pbc */
	    rvec_add(cg_cm[cg],box[dim],comm->buf_vr[nsend]);
	  } else {
	    copy_rvec(cg_cm[cg],comm->buf_vr[nsend]);
	  }
	  nsend++;
	  nat += cgindex[cg+1] - cgindex[cg];
	}
      }
    }
    ind->nsend[ncell]   = nsend;
    ind->nsend[ncell+1] = nat;
    /* Communicate the number of cg's and atoms to receive */
    dd_sendrecv_int(dd, dim_ind, ddBackward,
		    ind->nsend, ncell+2,
		    ind->nrecv, ncell+2);
    /* Communicate the global cg indices, receive in place */
    if (ncg_cell[ncell] + ind->nrecv[ncell] > dd->cg_nalloc
	|| dd->cg_nalloc == 0) {
      dd->cg_nalloc = over_alloc(ncg_cell[ncell] + ind->nrecv[ncell]);
      srenew(index_gl,dd->cg_nalloc);
      srenew(cgindex,dd->cg_nalloc+1);
    }
    dd_sendrecv_int(dd, dim_ind, ddBackward,
		    comm->buf_int,            nsend,
		    index_gl+ncg_cell[ncell], ind->nrecv[ncell]);
    /* Communicate cg_cm, receive in place */
    if (ncg_cell[ncell] + ind->nrecv[ncell] > fr->cg_nalloc) {
      dd_realloc_fr_cg(fr,ncg_cell[ncell] + ind->nrecv[ncell]);
      cg_cm = fr->cg_cm;
    }
    dd_sendrecv_rvec(dd, dim_ind, ddBackward,
		     comm->buf_vr,          nsend,
		     cg_cm+ncg_cell[ncell], ind->nrecv[ncell]);
    /* Make the charge group index */
    for(cell=ncell; cell<2*ncell; cell++) {
      ncg_cell[cell+1] = ncg_cell[cell] + ind->nrecv[cell-ncell];
      for(cg=ncg_cell[cell]; cg<ncg_cell[cell+1]; cg++) {
	cg_gl = index_gl[cg];
	nrcg = gcgs->index[cg_gl+1] - gcgs->index[cg_gl];
	cgindex[cg+1] = cgindex[cg] + nrcg;
	nat_tot += nrcg;
      }
    }
    ncell += ncell;
  }
  dd->index_gl = index_gl;
  dd->cgindex  = cgindex;

  dd->ncg_tot = ncg_cell[dd->ncell];
  dd->nat_tot       = nat_tot;
  dd->nat_tot_vsite = nat_tot;
  dd->nat_tot_con   = nat_tot;

  if (debug) {
    fprintf(debug,"Finished setting up DD communication, cells:");
    for(c=0; c<dd->ncell; c++)
      fprintf(debug," %d",dd->ncg_cell[c+1]-dd->ncg_cell[c]);
    fprintf(debug,"\n");
  }
}

static void set_cg_boundaries(gmx_domdec_t *dd)
{
  int c;

  for(c=0; c<dd->nicell; c++) {
    dd->icell[c].cg1  = dd->ncg_cell[c+1];
    dd->icell[c].jcg0 = dd->ncg_cell[dd->icell[c].j0];
    dd->icell[c].jcg1 = dd->ncg_cell[dd->icell[c].j1];
  }
}

void dd_partition_system(FILE         *fplog,
			 int          step,
			 t_commrec    *cr,
			 bool         bMasterState,
			 t_state      *state_global,
			 t_topology   *top_global,
			 t_inputrec   *ir,
			 t_state      *state_local,
			 rvec         **f,
			 rvec         **buf,
			 t_mdatoms    *mdatoms,
			 t_topology   *top_local,
			 t_forcerec   *fr,
			 gmx_constr_t *constr,
			 t_nrnb       *nrnb,
			 gmx_wallcycle_t wcycle,
			 bool         bVerbose)
{
  gmx_domdec_t *dd;
  int  i,j,cg0=0;
  bool bLogLoad;

  dd = cr->dd;
  
  /* Check if we have recorded loads on the nodes */
  if (dd->comm->bRecordLoad && dd_have_load(dd->comm)) {
    /* Print load every nstlog, first and last step to the log file */
    bLogLoad = ((ir->nstlog > 0 && step % ir->nstlog == 0) ||
		!dd->comm->bFirstPrinted ||
		(step + ir->nstlist > ir->init_step + ir->nsteps));
    if (dd->bDynLoadBal || bLogLoad || bVerbose) {
      get_load_distribution(dd,wcycle);
      if (DDMASTER(dd)) {
	if (bLogLoad)
	  dd_print_load(fplog,dd,step-1);
	if (bVerbose)
	  dd_print_load_verbose(dd);
      }
      dd->comm->bFirstPrinted = TRUE;
    }
  }

  if (bMasterState) {
    get_cg_distribution(fplog,step,dd,
			&top_global->blocks[ebCGS],
			state_global->box,state_global->x);
    
    dd_distribute_state(dd,&top_global->blocks[ebCGS],
			state_global,state_local,f,buf);
    
    dd_make_local_cgs(dd,&top_local->blocks[ebCGS]);

    if (dd->ncg_home > fr->cg_nalloc)
      dd_realloc_fr_cg(fr,dd->ncg_home);
    calc_cgcm(fplog,0,dd->ncg_home,
	      &top_local->blocks[ebCGS],state_local->x,fr->cg_cm);
    
    inc_nrnb(nrnb,eNR_CGCM,dd->nat_home);

    cg0 = 0;
  }

  set_dd_cell_sizes(dd,state_local->box,DYNAMIC_BOX(*ir),bMasterState,FALSE,
		    step);
  if (nstDDDumpGrid > 0 && step % nstDDDumpGrid == 0)
    write_dd_grid_pdb("dd_grid",step,dd,state_local->box);

  if (!bMasterState) {
    cg0 = dd_redistribute_cg(fplog,step,dd,&top_global->blocks[ebCGS],
			     state_local,f,buf,fr,mdatoms,nrnb);
  }

  /* Setup up the communication and communicate the coordinates */
  setup_dd_communication(fplog,step,dd,&top_global->blocks[ebCGS],
			 state_local->box,fr);
  
  /* Set the indices */
  make_dd_indices(dd,&top_global->blocks[ebCGS],cg0,fr);

  /* Set the charge group boundaries for neighbor searching */
  set_cg_boundaries(dd);

  /* Update the rest of the forcerec */
  fr->cg0 = 0;
  fr->hcg = dd->ncg_tot;
  if (fr->bTwinRange) {
    fr->f_twin_n = dd->nat_tot;
    if (fr->f_twin_n > fr->f_twin_nalloc) {
      fr->f_twin_nalloc = over_alloc(fr->f_twin_n);
      srenew(fr->f_twin,fr->f_twin_nalloc);
    }
  }
  if (EEL_FULL(fr->eeltype)) {
    fr->f_el_recip_n = (dd->n_intercg_excl ? dd->nat_tot : dd->nat_home);
    if (fr->f_el_recip_n > fr->f_el_recip_nalloc) {
      fr->f_el_recip_nalloc = over_alloc(fr->f_el_recip_n);
      srenew(fr->f_el_recip,fr->f_el_recip_nalloc);
    }
  }

  /* Extract a local topology from the global topology */
  dd_make_local_top(fplog,dd,fr,top_global,top_local);

  if (top_global->idef.il[F_CONSTR].nr > 0) {
    dd_make_local_constraints(dd,top_global->idef.il[F_CONSTR].iatoms,
			      ir->nProjOrder);
  } else {
    dd->nat_tot_con = dd->nat_tot_vsite;
  }
  /* Make space for the extra coordinates for virtual site
   * or constraint communication.
   */
  if (dd->nat_tot_con > state_local->nalloc)
    dd_realloc_state(state_local,f,buf,dd->nat_tot_con);

  /* We make the all mdatoms up to nat_tot_con.
   * We could save some work by only setting invmass
   * between nat_tot and nat_tot_con.
   */
  /* This call also sets the new number of home particles to dd->nat_home */
  atoms2md(&top_global->atoms,ir,top_global->idef.il[F_ORIRES].nr,
	   dd->nat_tot_con,dd->gatindex,0,dd->nat_home,mdatoms);

  if (!(cr->duty & DUTY_PME))
    /* Send the charges to our PME only node */
    gmx_pme_send_x_q(cr,state_local->box,NULL,
		     mdatoms->chargeA,mdatoms->chargeB,
		     mdatoms->nChargePerturbed,0,FALSE);

  if (dd->constraints || top_global->idef.il[F_SETTLE].nr>0)
    set_constraints(fplog,constr,top_global,ir,mdatoms,dd);

  if (ir->pull.ePull != epullNO)
    /* Update the local pull groups */
    dd_make_local_pull_groups(dd,&ir->pull,mdatoms);

  /* We need the constructing atom coordinates of the virtual sites
   * when spreading the forces.
   */
  dd_move_x_vsites(dd,state_local->box,state_local->x);

  clear_dd_cycle_counts(dd);

  if (nstDDDump > 0 && step % nstDDDump == 0) {
    dd_move_x(dd,state_local->box,state_local->x,*buf);
    write_dd_pdb("dd_dump",step,"dump",&top_global->atoms,dd,dd->nat_tot,
		 state_local->x,state_local->box);
  }
}
