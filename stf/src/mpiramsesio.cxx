/*! \file mpiramsesio.cxx
 *  \brief this file contains routines used with MPI compilation and ramses io and domain construction. 
 */

#ifdef USEMPI

//-- For MPI

#include "stf.h"

#include "ramsesitems.h"
#include "endianutils.h"

/// \name RAMSES Domain decomposition
//@{

/*! 
    Determine the domain decomposition.\n
    Here the domains are constructured in data units
    only ThisTask==0 should call this routine. It is tricky to get appropriate load balancing and correct number of particles per processor.\n

    I could use recursive binary splitting like kd-tree along most spread axis till have appropriate number of volumes corresponding 
    to number of processors.

    NOTE: assume that cannot store data so position information is read Nsplit times to determine boundaries of subvolumes
    could also randomly subsample system and produce tree from that 
    should store for each processor the node structure generated by the domain decomposition
    what I could do is read file twice, one to get extent and other to calculate entropy then decompose
    along some primary axis, then choose orthogonal axis, keep iterating till have appropriate number of subvolumes
    then store the boundaries of the subvolume. this means I don't store data but get at least reasonable domain decomposition

    NOTE: pkdgrav uses orthoganal recursive bisection along with kd-tree, gadget-2 uses peno-hilbert curve to map particles and oct-trees 
    the question with either method is guaranteeing load balancing. for ORB achieved by splitting (sub)volume along a dimension (say one with largest spread or max entropy)
    such that either side of the cut has approximately the same number of particles (ie: median splitting). But for both cases, load balancing requires particle information
    so I must load the system then move particles about to ensure load balancing.

    Main thing first is get the dimensional extent of the system.
    then I could get initial splitting just using mid point between boundaries along each dimension.
    once have that initial splitting just load data then start shifting data around.
*/
void MPIDomainExtentRAMSES(Options &opt){
    Int_t i;
    char buf[2000];
    fstream Framses;
    RAMSES_Header ramses_header_info;
    string stringbuf;
    if (ThisTask==0) {
    sprintf(buf,"info_%s.txt",opt.fname);
    Framses.open(buf, ios::in);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    getline(Framses,stringbuf);
    Framses>>stringbuf>>stringbuf>>ramses_header_info.BoxSize;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.time;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.aexp;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.HubbleParam;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.Omegam;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.OmegaLambda;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.Omegak;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.Omegab;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.scale_l;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.scale_d;
    Framses>>stringbuf>>stringbuf>>ramses_header_info.scale_t;
    Framses.close();
    ///note that code units are 0 to 1 
    for (int j=0;j<3;j++) {mpi_xlim[j][0]=0;mpi_xlim[j][1]=1.0;}

    //There may be issues with particles exactly on the edge of a domain so before expanded limits by a small amount
    //now only done if a specific compile option passed
#ifdef MPIEXPANDLIM
    for (int j=0;j<3;j++) {
        Double_t dx=0.001*(mpi_xlim[j][1]-mpi_xlim[j][0]);
        mpi_xlim[j][0]-=dx;mpi_xlim[j][1]+=dx;
    }
#endif
    }

    //make sure limits have been found
    MPI_Barrier(MPI_COMM_WORLD);
    if (NProcs==1) {
        for (i=0;i<3;i++) {
            mpi_domain[ThisTask].bnd[i][0]=mpi_xlim[i][0];
            mpi_domain[ThisTask].bnd[i][1]=mpi_xlim[i][1];
        }
    }
}

void MPIDomainDecompositionRAMSES(Options &opt){
#define SKIP2 Fgad[i].read((char*)&dummy, sizeof(dummy));
    Int_t i,j,k,n,m;
    int Nsplit,isplit;

    if (ThisTask==0) {
    //determine the number of splits in each dimension
    Nsplit=log((float)NProcs)/log(2.0);
    mpi_ideltax[0]=0;mpi_ideltax[1]=1;mpi_ideltax[2]=2;
    isplit=0;
    for (j=0;j<3;j++) mpi_nxsplit[j]=0;
    for (j=0;j<Nsplit;j++) {
        mpi_nxsplit[mpi_ideltax[isplit++]]++;
        if (isplit==3) isplit=0;
    }
    for (j=0;j<3;j++) mpi_nxsplit[j]=pow(2.0,mpi_nxsplit[j]);
    //for all the cells along the boundary of axis with the third split axis (smallest variance)
    //set the domain limits to the sims limits
    int ix=mpi_ideltax[0],iy=mpi_ideltax[1],iz=mpi_ideltax[2];
    int mpitasknum;
    for (j=0;j<mpi_nxsplit[iy];j++) {
        for (i=0;i<mpi_nxsplit[ix];i++) {
            mpitasknum=i+j*mpi_nxsplit[ix]+0*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[iz][0]=mpi_xlim[iz][0];
            mpitasknum=i+j*mpi_nxsplit[ix]+(mpi_nxsplit[iz]-1)*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[iz][1]=mpi_xlim[iz][1];
        }
    }
    //here for domains along second axis
    for (k=0;k<mpi_nxsplit[iz];k++) {
        for (i=0;i<mpi_nxsplit[ix];i++) {
            mpitasknum=i+0*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[iy][0]=mpi_xlim[iy][0];
            mpitasknum=i+(mpi_nxsplit[iy]-1)*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[iy][1]=mpi_xlim[iy][1];
        }
    }
    //finally along axis with largest variance
    for (k=0;k<mpi_nxsplit[iz];k++) {
        for (j=0;j<mpi_nxsplit[iy];j++) {
            mpitasknum=0+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[ix][0]=mpi_xlim[ix][0];
            mpitasknum=(mpi_nxsplit[ix]-1)+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
            mpi_domain[mpitasknum].bnd[ix][1]=mpi_xlim[ix][1];
        }
    }
    //here use the three different histograms to define the boundary
    int start[3],end[3];
    Double_t bndval[3],binsum[3],lastbin;
    start[0]=start[1]=start[2]=0;
    for (i=0;i<mpi_nxsplit[ix];i++) {
        bndval[0]=(mpi_xlim[ix][1]-mpi_xlim[ix][0])*(Double_t)(i+1)/(Double_t)mpi_nxsplit[ix];
        if(i<mpi_nxsplit[ix]-1) {
        for (j=0;j<mpi_nxsplit[iy];j++) {
            for (k=0;k<mpi_nxsplit[iz];k++) {
                //define upper limit 
                mpitasknum=i+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[ix][1]=bndval[0];
                //define lower limit
                mpitasknum=(i+1)+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[ix][0]=bndval[0];
            }
        }
        }
        //now for secondary splitting
        if (mpi_nxsplit[iy]>1) 
        for (j=0;j<mpi_nxsplit[iy];j++) {
            bndval[1]=(mpi_xlim[iy][1]-mpi_xlim[iy][0])*(Double_t)(j+1)/(Double_t)mpi_nxsplit[iy];
            if(j<mpi_nxsplit[iy]-1) {
            for (k=0;k<mpi_nxsplit[iz];k++) {
                mpitasknum=i+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[iy][1]=bndval[1];
                mpitasknum=i+(j+1)*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[iy][0]=bndval[1];
            }
            }
            if (mpi_nxsplit[iz]>1)
            for (k=0;k<mpi_nxsplit[iz];k++) {
                bndval[2]=(mpi_xlim[iz][1]-mpi_xlim[iz][0])*(Double_t)(k+1)/(Double_t)mpi_nxsplit[iz];
                if (k<mpi_nxsplit[iz]-1){
                mpitasknum=i+j*mpi_nxsplit[ix]+k*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[iz][1]=bndval[2];
                mpitasknum=i+j*mpi_nxsplit[ix]+(k+1)*(mpi_nxsplit[ix]*mpi_nxsplit[iy]);
                mpi_domain[mpitasknum].bnd[iz][0]=bndval[2];
                }
            }
        }
    }
    }
}

///reads a gadget file to determine number of particles in each MPIDomain
///\todo need to add codeo to read positions of particles/gas cells and send them to the appropriate mpi thead
void MPINumInDomainRAMSES(Options &opt)
{
    MPIDomainExtentRAMSES(opt);
    if (NProcs>1) {
    MPIDomainDecompositionRAMSES(opt);
    MPIInitialDomainDecomposition();
    Int_t i,j,k,n,m,temp,Ntot,indark,ingas,instar;
    Int_t idval;
    int dummy;
    Int_t Nlocalold=Nlocal;

    MPI_Status status;
    Int_t Nlocalbuf,ibuf=0,*Nbuf, *Nbaryonbuf;
    if (ThisTask==0) {
        Nbuf=new Int_t[NProcs];
        Nbaryonbuf=new Int_t[NProcs];
        for (int j=0;j<NProcs;j++) Nbuf[j]=0;
        for (int j=0;j<NProcs;j++) Nbaryonbuf[j]=0;
    }
    if (ThisTask==0) {
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (ThisTask==0) {
        Nlocal=Nbuf[ThisTask];
        for(ibuf = 1; ibuf < NProcs; ibuf++)
            MPI_Ssend(&Nbuf[ibuf],1,MPI_Int_t, ibuf, ibuf+NProcs, MPI_COMM_WORLD);
    }
    else {
        MPI_Recv(&Nlocal, 1, MPI_Int_t, 0, ThisTask+NProcs, MPI_COMM_WORLD, &status);
    }
    if (opt.iBaryonSearch) {
        if (ThisTask==0) {
            Nlocalbaryon[0]=Nbaryonbuf[ThisTask];
            for(ibuf = 1; ibuf < NProcs; ibuf++)
                MPI_Ssend(&Nbaryonbuf[ibuf],1,MPI_Int_t, ibuf, ibuf+NProcs, MPI_COMM_WORLD);
        }
        else {
            MPI_Recv(&Nlocalbaryon[0], 1, MPI_Int_t, 0, ThisTask+NProcs, MPI_COMM_WORLD, &status);
        }
    }
#ifdef MPIREDUCEMEM
    Nmemlocal=Nlocal*(1.0+MPIExportFac);
    if (opt.iBaryonSearch) Nmemlocalbaryon=Nlocalbaryon[0]*(1.0+MPIExportFac);
#else
    Nlocal*=(1.0+MPIExportFac);
#endif
    }
}

//@}

#endif