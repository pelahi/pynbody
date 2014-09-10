#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "smooth.h"
#include "kd.h"



int smInit(SMX *psmx,KD kd,int nSmooth,float *fPeriod)
{
	SMX smx;
	KDN *root;
	int pi,j;
	int bError=0;

	root = &kd->kdNodes[ROOT];
	assert(root != NULL);
	/*
	 ** Check to make sure that the bounds of the simulation agree
	 ** with the period specified, if not cause an error.
	 */
	for (j=0;j<3;++j) {
		if (root->bnd.fMax[j] - root->bnd.fMin[j] > fPeriod[j]) {
			fprintf(stderr,"ERROR(smInit):Bounds of the simulation volume exceed ");
			fprintf(stderr,"the period specified in the %c-dimension. (%f > %f)\n",'x'+j, root->bnd.fMax[j] - root->bnd.fMin[j], fPeriod[j]);
			bError = 1;
			}
		}
	if (bError) exit(1);
	assert(nSmooth <= kd->nActive);
	smx = (SMX)malloc(sizeof(struct smContext)); assert(smx != NULL);
	smx->kd = kd;
	smx->nSmooth = nSmooth;
	smx->pq = (PQ *)malloc(nSmooth*sizeof(PQ));                     assert(smx->pq != NULL);
	PQ_INIT(smx->pq,nSmooth);
	smx->iMark = (char *)malloc(kd->nActive*sizeof(char));          assert(smx->iMark != NULL);
	smx->nListSize = smx->nSmooth+RESMOOTH_SAFE;
	smx->fList = (float *)malloc(smx->nListSize*sizeof(float));     assert(smx->fList != NULL);
	smx->pList = (int *)malloc(smx->nListSize*sizeof(int));         assert(smx->pList != NULL);

	for (j=0;j<3;++j) smx->fPeriod[j] = fPeriod[j];


#ifdef KDT_THREADING
	smx->nCurrent=0;

	pthread_mutexattr_t Attr;

	pthread_mutexattr_init(&Attr);
	pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE);

	smx->pMutex = malloc(sizeof(pthread_mutex_t));

	if (pthread_mutex_init(smx->pMutex, &Attr) != 0)
	{
		free(smx->pMutex);
    free(smx);
    return(0);
  }

	smx->pReady = malloc(sizeof(pthread_cond_t));

	if (pthread_cond_init(smx->pReady,NULL)!=0) {
		free(smx->pMutex);
		free(smx->pReady);
		free(smx);
		return(0);
	}

	smx->nLocals=0;
	smx->nReady=0;

	smx->smx_global=NULL;

#endif
	*psmx = smx;
	return(1);
	}

#ifdef KDT_THREADING

SMX smInitThreadLocalCopy(SMX from) {

	SMX smx;
	KDN *root;
	int pi,j;
	int bError=0;

	root = &from->kd->kdNodes[ROOT];

	smx = (SMX)malloc(sizeof(struct smContext)); assert(smx != NULL);
	smx->kd = from->kd;
	smx->nSmooth = from->nSmooth;
	smx->pq = (PQ *)malloc(from->nSmooth*sizeof(PQ));                     assert(smx->pq != NULL);
	PQ_INIT(smx->pq,from->nSmooth);
	smx->iMark = (char *)malloc(from->kd->nActive*sizeof(char));          assert(smx->iMark != NULL);
	smx->nListSize = from->nListSize;
	smx->fList = (float *)malloc(smx->nListSize*sizeof(float));     assert(smx->fList != NULL);
	smx->pList = (int *)malloc(smx->nListSize*sizeof(int));         assert(smx->pList != NULL);
	for (j=0;j<3;++j) smx->fPeriod[j] = from->fPeriod[j];
	for (pi=0;pi<smx->kd->nActive;++pi) {
		smx->iMark[pi] = 0;
	}
	smx->pMutex = from->pMutex;
	smx->pReady = from->pReady;
	from->nLocals++;

	smx->smx_global = from;
	smx->nCurrent = 0;
	smx->nLocals=0;
	smInitPriorityQueue(smx);
	return smx;
}

void smFinishThreadLocalCopy(SMX smx)
{
	smx->smx_global->nLocals--;

	free(smx->pq);
	free(smx->fList);
	free(smx->pList);
	free(smx->iMark);
	free(smx);
}

void smReset(SMX smx_local) {
	SMX smxg = smx_local->smx_global;

	pthread_mutex_lock(smxg->pMutex);
	smxg->nReady++;
	if(smxg->nReady==smxg->nLocals) {
		smxg->nReady=0;
		smxg->nCurrent=0;
		pthread_cond_broadcast(smxg->pReady);
	} else {
		pthread_cond_wait(smxg->pReady,smxg->pMutex);
	}
	pthread_mutex_unlock(smxg->pMutex);
	smx_local->nCurrent=0;
}

#define WORKUNIT 1000

int smGetNext(SMX smx_local) {
	int i = smx_local->nCurrent;

	if(smx_local->nCurrent%WORKUNIT==0) {
		// we have reached the end of a work unit. Get and increment the global
		// counter.
		pthread_mutex_lock(smx_local->pMutex);
		smx_local->nCurrent=smx_local->smx_global->nCurrent;
		i = smx_local->nCurrent;
		smx_local->smx_global->nCurrent+=WORKUNIT;
		pthread_mutex_unlock(smx_local->pMutex);
	}

	// i now has the next thing to be processed
	// increment the local counter

	smx_local->nCurrent+=1;

	return i;
}

#endif

void smFinish(SMX smx)
{
	free(smx->iMark);
	free(smx->pq);
	free(smx->fList);
	free(smx->pList);

#ifdef KDT_THREADING
	pthread_mutex_destroy(smx->pMutex);
	pthread_cond_destroy(smx->pReady);
	free(smx->pMutex);
	free(smx->pReady);
#endif

	free(smx);

}



void smBallSearch(SMX smx,float fBall2,float *ri)
{
	KDN *c;
	PARTICLE *p;
	KD kd;
	int cell,cp,ct,pj;
	float fDist2,dx,dy,dz,lx,ly,lz,sx,sy,sz,x,y,z;
	PQ *pq;

	kd = smx->kd;
	c = smx->kd->kdNodes;
	p = smx->kd->p;
	pq = smx->pqHead;
	x = ri[0];
	y = ri[1];
	z = ri[2];
	lx = smx->fPeriod[0];
	ly = smx->fPeriod[1];
	lz = smx->fPeriod[2];
	cell = ROOT;
	/*
	 ** First find the "local" Bucket.
	 ** This could mearly be the closest bucket to ri[3].
	 */
	while (cell < smx->kd->nSplit) {
		if (ri[c[cell].iDim] < c[cell].fSplit) cell = LOWER(cell);
		else cell = UPPER(cell);
		}
	/*
	 ** Now start the search from the bucket given by cell!
	 */
	for (pj=c[cell].pLower;pj<=c[cell].pUpper;++pj) {
		dx = x - GET2(kd->pNumpyPos,p[pj].iOrder,0);
		dy = y - GET2(kd->pNumpyPos,p[pj].iOrder,1);
		dz = z - GET2(kd->pNumpyPos,p[pj].iOrder,2);
		fDist2 = dx*dx + dy*dy + dz*dz;
		if (fDist2 < fBall2) {
			if (smx->iMark[pj]) continue;
			smx->iMark[pq->p] = 0;
			smx->iMark[pj] = 1;
			pq->fKey = fDist2;
			pq->p = pj;
			pq->ax = 0.0;
			pq->ay = 0.0;
			pq->az = 0.0;
			PQ_REPLACE(pq);
			fBall2 = pq->fKey;
			}
		}
	while (cell != ROOT) {
		cp = SIBLING(cell);
		ct = cp;
		SETNEXT(ct,ROOT);
		while (1) {
			INTERSECT(c,cp,fBall2,lx,ly,lz,x,y,z,sx,sy,sz);
			/*
			 ** We have an intersection to test.
			 */
			if (cp < smx->kd->nSplit) {
				cp = LOWER(cp);
				continue;
				}
			else {
				for (pj=c[cp].pLower;pj<=c[cp].pUpper;++pj) {
					dx = sx - GET2(kd->pNumpyPos,p[pj].iOrder,0);
					dy = sy - GET2(kd->pNumpyPos,p[pj].iOrder,1);
					dz = sz - GET2(kd->pNumpyPos,p[pj].iOrder,2);
					fDist2 = dx*dx + dy*dy + dz*dz;
					if (fDist2 < fBall2) {
						if (smx->iMark[pj]) continue;
						smx->iMark[pq->p] = 0;
						smx->iMark[pj] = 1;
						pq->fKey = fDist2;
						pq->p = pj;
						pq->ax = sx - x;
						pq->ay = sy - y;
						pq->az = sz - z;
						PQ_REPLACE(pq);
						fBall2 = pq->fKey;
						}
					}
				}
		GetNextCell:
			SETNEXT(cp,ROOT);
			if (cp == ct) break;
			}
		cell = PARENT(cell);
		}
	smx->pqHead = pq;
	}


int smBallGather(SMX smx,float fBall2,float *ri)
{
	KDN *c;
	PARTICLE *p;
	KD kd=smx->kd;
	int pj,nCnt,cp,nSplit;
	float dx,dy,dz,x,y,z,lx,ly,lz,sx,sy,sz,fDist2;

	c = smx->kd->kdNodes;
	p = smx->kd->p;
	nSplit = smx->kd->nSplit;
	lx = smx->fPeriod[0];
	ly = smx->fPeriod[1];
	lz = smx->fPeriod[2];
	x = ri[0];
	y = ri[1];
	z = ri[2];
	nCnt = 0;
	cp = ROOT;
	while (1) {
		INTERSECT(c,cp,fBall2,lx,ly,lz,x,y,z,sx,sy,sz);
		/*
		 ** We have an intersection to test.
		 */
		if (cp < nSplit) {
			cp = LOWER(cp);
			continue;
			}
		else {
			for (pj=c[cp].pLower;pj<=c[cp].pUpper;++pj) {
				dx = sx - GET2(kd->pNumpyPos,p[pj].iOrder,0);
				dy = sy - GET2(kd->pNumpyPos,p[pj].iOrder,1);
				dz = sz - GET2(kd->pNumpyPos,p[pj].iOrder,2);
				fDist2 = dx*dx + dy*dy + dz*dz;
				if (fDist2 <= fBall2) {
				  if(nCnt>=smx->nListSize) {
				    if(!smx->warnings) fprintf(stderr, "Smooth - particle cache too small for local density - results will be incorrect\n");
				    smx->warnings=true;
				    break;
				  }
				  smx->fList[nCnt] = fDist2;
				  smx->pList[nCnt++] = pj;

				}

			}

			}
	GetNextCell:
		SETNEXT(cp,ROOT);
		if (cp == ROOT) break;
		}
	assert(nCnt <= smx->nListSize);
	return(nCnt);
	}





void smSmoothInitStep(SMX smx, int nProcs_for_smooth)
{

	PARTICLE *p;
	int pi;
	KD kd=smx->kd;

	for (pi=0;pi<kd->nActive;++pi) {
		smx->iMark[pi] = 0;
	}

	smInitPriorityQueue(smx);
}

void smDomainDecomposition(KD kd, int nprocs) {

	// AP 31/8/2014 - Here is the domain decomposition for nProcs>1
	// In principle one should do better by localizing the
	// domains -- the current approach is a seriously naive decomposition.
	// This will result in more snake collisions than necessary.
	// However in practice, up to nCpu = 8, the scaling is looking
	// pretty linear anyway so I'm leaving that for future.

	PARTICLE *p;
	int pi;

	if(nprocs>0) {
		for (pi=0;pi<kd->nActive;++pi) {
			SETSMOOTH(pi,-(float)(1+pi%nprocs));
		}
	}
}

void smInitPriorityQueue(SMX smx) {
	/*
	 ** Initialize Priority Queue.
	 */

	PARTICLE *p;
	PQ *pq,*pqLast;
	int pi,pin,pj,pNext,nSmooth;
	float ax,ay,az;

	pqLast = &smx->pq[smx->nSmooth-1];
	nSmooth = smx->nSmooth;
	pin = 0;
	pNext = 1;
	ax = 0.0;
	ay = 0.0;
	az = 0.0;

	for (pq=smx->pq,pj=0;pq<=pqLast;++pq,++pj) {
		smx->iMark[pj] = 1;
		pq->p = pj;
		pq->ax = ax;
		pq->ay = ay;
		pq->az = az;
	}
  smx->pin = pin;
  smx->pNext = pNext;
  smx->ax = ax;
  smx->ay = ay;
  smx->az = az;
}


int smSmoothStep(SMX smx,void (*fncSmooth)(SMX,int,int,int *,float *), int procid)
{
	KDN *c;
	PARTICLE *p;
	PQ *pq,*pqLast;
	KD kd=smx->kd;
	int cell;
	int pi,pin,pj,pNext,nCnt,nSmooth;
	int nScanned=0;

	float dx,dy,dz,x,y,z,h2,ax,ay,az;
	float proc_signal = -(float)(procid)-1.0;
	float ri[3];

	c = smx->kd->kdNodes;
	p = smx->kd->p;
	pqLast = &smx->pq[smx->nSmooth-1];
	nSmooth = smx->nSmooth;
	pin = smx->pin;
	pNext = smx->pNext;
	ax = smx->ax;
	ay = smx->ay;
	az = smx->az;


	if (GETSMOOTH(pin) >= 0) {
		// the first particle we are supposed to smooth is
		// actually already done. We need to search for another
		// suitable candidate. Preferably a long way away from other
		// threads, if this is threaded.


		while (GETSMOOTH(pNext) != proc_signal) {
		 		++pNext;
				++nScanned;
				if(pNext>=smx->kd->nActive)
					pNext=0;
				if(nScanned==smx->kd->nActive) {
					// Nothing remains to be done.
					// pthread_mutex_unlock(smx->pMutex);
					return -1;
				}
		}

		// mark the particle as 'processed' by assigning a dummy positive value
		// N.B. a race condition here doesn't matter since duplicating a bit of
		// work is more efficient than using a mutex (verified).
		SETSMOOTH(pNext,10);

		pi = pNext;
		++pNext;
		x = GET2(kd->pNumpyPos,p[pi].iOrder,0);
		y = GET2(kd->pNumpyPos,p[pi].iOrder,1);
		z = GET2(kd->pNumpyPos,p[pi].iOrder,2);
		/*
		** First find the "local" Bucket.
		** This could merely be the closest bucket to ri[3].
		*/
		cell = ROOT;
		while (cell < smx->kd->nSplit) {
			if (GET2(kd->pNumpyPos,p[pi].iOrder,c[cell].iDim) <c[cell].fSplit)
				cell = LOWER(cell);
			else
				cell = UPPER(cell);
		}
		/*
		** Remove everything from the queue.
		*/
		smx->pqHead = NULL;
		for (pq=smx->pq;pq<=pqLast;++pq) smx->iMark[pq->p] = 0;
		/*
		** Add everything from pj up to and including pj+nSmooth-1.
		*/
		pj = c[cell].pLower;
		if (pj > smx->kd->nActive - nSmooth)
			pj = smx->kd->nActive - nSmooth;
		for (pq=smx->pq;pq<=pqLast;++pq) {
			smx->iMark[pj] = 1;
			dx = x - GET2(kd->pNumpyPos,p[pj].iOrder,0);
			dy = y - GET2(kd->pNumpyPos,p[pj].iOrder,1);
			dz = z - GET2(kd->pNumpyPos,p[pj].iOrder,2);
			pq->fKey = dx*dx + dy*dy + dz*dz;
			pq->p = pj++;
			pq->ax = 0.0;
			pq->ay = 0.0;
			pq->az = 0.0;
		}
		PQ_BUILD(smx->pq,nSmooth,smx->pqHead);
	} else {
		// Calculate priority queue using existing particles
		pi = pin;

		// Mark - see comment above
		SETSMOOTH(pi,10);

		x = GET2(kd->pNumpyPos,p[pi].iOrder,0);
		y = GET2(kd->pNumpyPos,p[pi].iOrder,1);
		z = GET2(kd->pNumpyPos,p[pi].iOrder,2);

		smx->pqHead = NULL;
		for (pq=smx->pq;pq<=pqLast;++pq) {
			pq->ax -= ax;
			pq->ay -= ay;
			pq->az -= az;
			dx = x + pq->ax - GET2(kd->pNumpyPos,p[pq->p].iOrder,0);
			dy = y + pq->ay - GET2(kd->pNumpyPos,p[pq->p].iOrder,1);
			dz = z + pq->az - GET2(kd->pNumpyPos,p[pq->p].iOrder,2);
			pq->fKey = dx*dx + dy*dy + dz*dz;
		}
		PQ_BUILD(smx->pq,nSmooth,smx->pqHead);
		ax = 0.0;
		ay = 0.0;
		az = 0.0;
	}

	for(int j=0; j<3; ++j) {
		ri[j] = GET2(kd->pNumpyPos,p[pi].iOrder,j);
	}

	smBallSearch(smx,smx->pqHead->fKey,ri);
	SETSMOOTH(pi,0.5*sqrt(smx->pqHead->fKey));

	// p[pi].fSmooth = 0.5*sqrt(smx->pfBall2[pi]);
	/*
	** Pick next particle, 'pin'.
	** Create fList and pList for function 'fncSmooth'.
	*/
	pin = pi;
	nCnt = 0;
	h2 = smx->pqHead->fKey;
	for (pq=smx->pq;pq<=pqLast;++pq) {

		/* the next line is commented out because it results in the furthest
		particle being excluded from the nearest-neighbor list - this means
		that although the user requests 32 NN, she would get back 31. By
		including the furthest particle, the neighbor list is always 32 long */

		//if (pq == smx->pqHead) continue;
		if(nCnt>=smx->nListSize) {
			// no room left
			if(!smx->warnings) fprintf(stderr, "Smooth - particle cache too small for local density - results will be incorrect\n");
			smx->warnings = false;
			break;
		}

		smx->pList[nCnt] = pq->p;
		smx->fList[nCnt++] = pq->fKey;



		if (GETSMOOTH(pq->p) >= 0) continue; // already done, don't re-do


		if (pq->fKey < h2) {
			pin = pq->p;
			h2 = pq->fKey;
			ax = pq->ax;
			ay = pq->ay;
			az = pq->az;
		}

	}

	//(*fncSmooth)(smx,pi,nCnt,smx->pList,smx->fList);

	smx->pi = pi;
	smx->pin = pin;
	smx->pNext = pNext;
	smx->ax = ax;
	smx->ay = ay;
	smx->az = az;

	return nCnt;
}


void smDensitySym(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih;
	int i,pj;
	KD kd = smx->kd;

	ih = 1.0/GETSMOOTH(pi);
	ih2 = ih*ih;
  	fNorm = 0.5*M_1_PI*ih*ih2;

	for (i=0;i<nSmooth;++i) {
    	pj = pList[i];
		r2 = fList[i]*ih2;
                rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0 && !smx->warnings) {
		  fprintf(stderr, "Internal consistency error\n");
		  smx->warnings=true;
		}
		rs *= fNorm;
		ACCUM(kd->pNumpyDen,kd->p[pi].iOrder,rs*GET(kd->pNumpyMass,kd->p[pj].iOrder));
		ACCUM(kd->pNumpyDen,kd->p[pj].iOrder,rs*GET(kd->pNumpyMass,kd->p[pi].iOrder));
  }

}


void smDensity(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih;
	int j,pj,pi_iord ;
	KD kd = smx->kd;

	pi_iord = kd->p[pi].iOrder;
	ih = 1.0/GET(kd->pNumpySmooth, pi_iord);
	ih2 = ih*ih;
	fNorm = M_1_PI*ih*ih2;
	SET(kd->pNumpyDen,pi_iord,0.0);
	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		ACCUM(kd->pNumpyDen,pi_iord,rs*GET(kd->pNumpyMass,kd->p[pj].iOrder));
	}

}

void smMeanQty1D(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih,mass,rho;
	int j,pj,pi_iord ;
	KD kd = smx->kd;

	pi_iord = kd->p[pi].iOrder;
	ih = 1.0/GET(kd->pNumpySmooth, pi_iord);
	ih2 = ih*ih;
	fNorm = M_1_PI*ih*ih2;

	SET(kd->pNumpyQtySmoothed,pi_iord,0.0);

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		ACCUM(kd->pNumpyQtySmoothed,pi_iord,
			  rs*mass*GET(kd->pNumpyQty,kd->p[pj].iOrder)/rho);
	}

}

void smMeanQtyND(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih,mass,rho;
	int j,k,pj,pi_iord ;
	KD kd = smx->kd;

	pi_iord = kd->p[pi].iOrder;
	ih = 1.0/GET(kd->pNumpySmooth, pi_iord);
	ih2 = ih*ih;
	fNorm = M_1_PI*ih*ih2;

	for(k=0;k<3;++k)
		SET2(kd->pNumpyQtySmoothed,pi_iord,k,0.0);

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		for(k=0;k<3;++k) {
			ACCUM2(kd->pNumpyQtySmoothed,pi_iord,k,
			    rs*mass*GET2(kd->pNumpyQty,kd->p[pj].iOrder,k)/rho);
		}
	}

}

void smDispQtyND(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih,mass,rho;
	int j,k,pj,pi_iord ;
	KD kd = smx->kd;
	float mean[3], tdiff;

	pi_iord = kd->p[pi].iOrder;
	ih = 1.0/GET(kd->pNumpySmooth, pi_iord);
	ih2 = ih*ih;
	fNorm = M_1_PI*ih*ih2;



	SET(kd->pNumpyQtySmoothed,pi_iord,0.0);

	for(k=0;k<3;++k) {

		mean[k]=0;
	}

	// pass 1: find mean

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		for(k=0;k<3;++k)
			mean[k]+=rs*mass*GET2(kd->pNumpyQty,kd->p[pj].iOrder,k)/rho;
	}

	// pass 2: get variance

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		for(k=0;k<3;++k) {
			tdiff = mean[k]-GET2(kd->pNumpyQty,kd->p[pj].iOrder,k);
			ACCUM(kd->pNumpyQtySmoothed,pi_iord,
				rs*mass*tdiff*tdiff/rho);
		}
	}

	// finally: take square root to get dispersion

	SET(kd->pNumpyQtySmoothed,pi_iord,sqrt(GET(kd->pNumpyQtySmoothed,pi_iord)));

}

void smDispQty1D(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,ih,mass,rho;
	int j,pj,pi_iord ;
	KD kd = smx->kd;
	float mean, tdiff;

	pi_iord = kd->p[pi].iOrder;
	ih = 1.0/GET(kd->pNumpySmooth, pi_iord);
	ih2 = ih*ih;
	fNorm = M_1_PI*ih*ih2;



	SET(kd->pNumpyQtySmoothed,pi_iord,0.0);

	mean=0;

	// pass 1: find mean

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		mean+=rs*mass*GET(kd->pNumpyQty,kd->p[pj].iOrder)/rho;
	}

	// pass 2: get variance

	for (j=0;j<nSmooth;++j) {
		pj = pList[j];
		r2 = fList[j]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		if(rs<0) rs=0;
		rs *= fNorm;
		mass=GET(kd->pNumpyMass,kd->p[pj].iOrder);
		rho=GET(kd->pNumpyDen,kd->p[pj].iOrder);
		tdiff = mean-GET(kd->pNumpyQty,kd->p[pj].iOrder);
		ACCUM(kd->pNumpyQtySmoothed,pi_iord,rs*mass*tdiff*tdiff/rho);
	}

	// finally: take square root to get dispersion

	SET(kd->pNumpyQtySmoothed,pi_iord,sqrt(GET(kd->pNumpyQtySmoothed,pi_iord)));

}


/*

void smMeanVelSym(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = 0.5*M_1_PI*sqrt(ih2)*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		rs *= fNorm;
		for (j=0;j<3;++j) {
			smx->kd->p[pi].vMean[j] += rs*smx->kd->p[pj].fMass/
				smx->kd->p[pj].fDensity*smx->kd->p[pj].v[j];
			smx->kd->p[pj].vMean[j] += rs*smx->kd->p[pi].fMass/
				smx->kd->p[pi].fDensity*smx->kd->p[pi].v[j];
			}
		}
	}


void smDivvSym(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs;
	float r, rs1, dvdotdr, fNorm1;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = 0.5*M_1_PI*sqrt(ih2)*ih2;
	fNorm1 = fNorm*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		r = sqrt(r2);
		rs = 2.0 - r;
		if (r2 < 1.0) {
			rs1 = -3 + 2.25*r;
			}
		else {
			rs1 = -0.75*rs*rs/r;
			}
		rs1 *= fNorm1;
		dvdotdr = 0.0;
		for (j=0;j<3;++j) {
			dvdotdr += (smx->kd->p[pj].v[j] - smx->kd->p[pi].v[j])*
				(smx->kd->p[pj].r[j] - smx->kd->p[pi].r[j]);
			}
		smx->kd->p[pi].fDivv -= rs1*smx->kd->p[pj].fMass/
			smx->kd->p[pj].fDensity*dvdotdr;
		smx->kd->p[pj].fDivv -= rs1*smx->kd->p[pi].fMass/
			smx->kd->p[pi].fDensity*dvdotdr;
		}
	}


void smDivv(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs;
	float r, rs1, dvdotdr, fNorm1;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = M_1_PI*sqrt(ih2)*ih2;
	fNorm1 = fNorm*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		r = sqrt(r2);
		rs = 2.0 - r;
		if (r2 < 1.0) {
			rs1 = -3 + 2.25*r;
			}
		else {
			rs1 = -0.75*rs*rs/r;
			}
		rs1 *= fNorm1;
		dvdotdr = 0.0;
		for (j=0;j<3;++j) {
			dvdotdr += (smx->kd->p[pj].v[j] - smx->kd->p[pi].v[j])*
				(smx->kd->p[pj].r[j] - smx->kd->p[pi].r[j]);
			}
		smx->kd->p[pi].fDivv -= rs1*smx->kd->p[pj].fMass/
			smx->kd->p[pj].fDensity*dvdotdr;
		}
	}

void smVelDisp(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,tv2;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = M_1_PI*sqrt(ih2)*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		rs *= fNorm;
		tv2 = 0.0;
		for (j=0;j<3;++j) {
                  tv2 += (smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j])*
                    (smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j]);
                }
		smx->kd->p[pi].fVel2 += rs*smx->kd->p[pj].fMass/smx->kd->p[pj].fDensity*tv2;
        }
}



void smVelDispSym(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,tv2;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = 0.5*M_1_PI*sqrt(ih2)*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		rs *= fNorm;
		tv2 = 0.0;
		for (j=0;j<3;++j) {
			tv2 += (smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j])*
				(smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j]);
			}
		smx->kd->p[pi].fVel2 += rs*smx->kd->p[pj].fMass/
			smx->kd->p[pj].fDensity*tv2;
		tv2 = 0.0;
		for (j=0;j<3;++j) {
			tv2 += (smx->kd->p[pi].v[j] - smx->kd->p[pj].vMean[j])*
				(smx->kd->p[pi].v[j] - smx->kd->p[pj].vMean[j]);
			}
		smx->kd->p[pj].fVel2 += rs*smx->kd->p[pi].fMass/
			smx->kd->p[pi].fDensity*tv2;
		}
	}

void smVelDispNBSym(SMX smx,int pi,int nSmooth,int *pList,float *fList)
{
	float fNorm,ih2,r2,rs,tv2;
	float dr;
	int i,j,pj;

	ih2 = 4.0/smx->pfBall2[pi];
	fNorm = 0.5*M_1_PI*sqrt(ih2)*ih2;
	for (i=0;i<nSmooth;++i) {
		pj = pList[i];
		r2 = fList[i]*ih2;
		rs = 2.0 - sqrt(r2);
		if (r2 < 1.0) rs = (1.0 - 0.75*rs*r2);
		else rs = 0.25*rs*rs*rs;
		rs *= fNorm;
		tv2 = 0.0;
		for (j=0;j<3;++j) {
			dr = smx->kd->p[pj].r[j] - smx->kd->p[pi].r[j];
			tv2 += (smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j] -
					smx->kd->p[pi].fDivv*dr)*
				(smx->kd->p[pj].v[j] - smx->kd->p[pi].vMean[j] -
				 smx->kd->p[pi].fDivv*dr);
			}
		smx->kd->p[pi].fVel2 += rs*smx->kd->p[pj].fMass/
			smx->kd->p[pj].fDensity*tv2;
		smx->kd->p[pj].fVel2 += rs*smx->kd->p[pi].fMass/
			smx->kd->p[pi].fDensity*tv2;
		}
	}
*/
