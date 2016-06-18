/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "dcrop.h"
#include "curves.h"
#include "mytime.h"
#include "refreshmap.h"
#include "rt_math.h"
// "ceil" rounding
#define SKIPS(a,b) ((a) / (b) + ((a) % (b) > 0))
#include <fstream>

namespace rtengine
{

extern const Settings* settings;

Crop::Crop (ImProcCoordinator* parent, EditDataProvider *editDataProvider, bool isDetailWindow)
    : PipetteBuffer(editDataProvider), origCrop(NULL), laboCrop(NULL), labnCrop(NULL),
      cropImg(NULL), cbuf_real(NULL), cshmap(NULL), transCrop(NULL), cieCrop(NULL), cbuffer(NULL),
      updating(false), newUpdatePending(false), skip(10),
      cropx(0), cropy(0), cropw(-1), croph(-1),
      trafx(0), trafy(0), trafw(-1), trafh(-1),
      rqcropx(0), rqcropy(0), rqcropw(-1), rqcroph(-1),
      borderRequested(32), upperBorder(0), leftBorder(0),
      cropAllocated(false),
      cropImageListener(NULL), parent(parent), isDetailWindow(isDetailWindow)
{
    parent->crops.push_back (this);
}

Crop::~Crop ()
{

    MyMutex::MyLock cropLock(cropMutex);

    std::vector<Crop*>::iterator i = std::find (parent->crops.begin(), parent->crops.end(), this);

    if (i != parent->crops.end ()) {
        parent->crops.erase (i);
    }

    MyMutex::MyLock processingLock(parent->mProcessing);
    freeAll ();
}

void Crop::destroy ()
{
    MyMutex::MyLock lock(cropMutex);
    MyMutex::MyLock processingLock(parent->mProcessing);
    freeAll();
}

void Crop::setListener (DetailedCropListener* il)
{
    // We can make reads in the IF, because the mProcessing lock is only needed for change
    if (cropImageListener != il) {
        MyMutex::MyLock lock(cropMutex);
        cropImageListener = il;
    }
}

EditUniqueID Crop::getCurrEditID()
{
    EditSubscriber *subscriber = PipetteBuffer::dataProvider ? PipetteBuffer::dataProvider->getCurrSubscriber() : NULL;
    return subscriber ? subscriber->getEditID() : EUID_None;
}

/*
 * Delete the edit image buffer if there's no subscriber anymore.
 * If allocation has to be done, it is deferred to Crop::update
 */
void Crop::setEditSubscriber(EditSubscriber* newSubscriber)
{
    MyMutex::MyLock lock(cropMutex);

    // At this point, editCrop.dataProvider->currSubscriber is the old subscriber
    EditSubscriber *oldSubscriber = PipetteBuffer::dataProvider ? PipetteBuffer::dataProvider->getCurrSubscriber() : NULL;

    if (newSubscriber == NULL || (oldSubscriber != NULL && oldSubscriber->getPipetteBufferType() != newSubscriber->getPipetteBufferType())) {
        if (PipetteBuffer::imgFloatBuffer != NULL) {
            delete PipetteBuffer::imgFloatBuffer;
            PipetteBuffer::imgFloatBuffer = NULL;
        }

        if (PipetteBuffer::LabBuffer != NULL) {
            delete PipetteBuffer::LabBuffer;
            PipetteBuffer::LabBuffer = NULL;
        }

        if (PipetteBuffer::singlePlaneBuffer.getW() != -1) {
            PipetteBuffer::singlePlaneBuffer.flushData();
        }
    }

    // If oldSubscriber == NULL && newSubscriber != NULL && newSubscriber->getEditingType() == ET_PIPETTE-> the image will be allocated when necessary
}

void Crop::update (int todo)
{
    MyMutex::MyLock cropLock(cropMutex);

    ProcParams& params = parent->params;
//       CropGUIListener* cropgl;

    // No need to update todo here, since it has already been changed in ImprocCoordinator::updatePreviewImage,
    // and Crop::update ask to do ALL anyway

    // give possibility to the listener to modify crop window (as the full image dimensions are already known at this point)
    int wx, wy, ww, wh, ws;
    bool overrideWindow = false;

    if (cropImageListener) {
        overrideWindow = cropImageListener->getWindow (wx, wy, ww, wh, ws);
    }

    // re-allocate sub-images and arrays if their dimensions changed
    bool needsinitupdate = false;

    if (!overrideWindow) {
        needsinitupdate = setCropSizes (rqcropx, rqcropy, rqcropw, rqcroph, skip, true);
    } else {
        needsinitupdate = setCropSizes (wx, wy, ww, wh, ws, true);    // this set skip=ws
    }

    // it something has been reallocated, all processing steps have to be performed
    if (needsinitupdate || (todo & M_HIGHQUAL)) {
        todo = ALL;
    }

    // Tells to the ImProcFunctions' tool what is the preview scale, which may lead to some simplifications
    parent->ipf.setScale (skip);

    Imagefloat* baseCrop = origCrop;
    int widIm = parent->fw;//full image
    int heiIm = parent->fh;


    bool needstransform  = parent->ipf.needsTransform();

    if (todo & (M_INIT | M_LINDENOISE)) {
        MyMutex::MyLock lock(parent->minit);  // Also used in improccoord

        int tr = getCoarseBitMask(params.coarse);

        if (!needsinitupdate) {
            setCropSizes (rqcropx, rqcropy, rqcropw, rqcroph, skip, true);
        }

        //  printf("x=%d y=%d crow=%d croh=%d skip=%d\n",rqcropx, rqcropy, rqcropw, rqcroph, skip);
        //  printf("trafx=%d trafyy=%d trafwsk=%d trafHs=%d \n",trafx, trafy, trafw*skip, trafh*skip);
        //  printf("x=%d y=%d crow=%d croh=%d skip=%d cropW=%d cropH=%d leftb=%d upperb=%d\n", rqcropx, rqcropy, rqcropw, rqcroph, skip, cropw, croph, leftBorder, upperBorder);
        // printf("trafx=%d trafyy=%d trafwsk=%d trafHs=%d \n", trafx, trafy, trafw * skip, trafh * skip);


        Imagefloat *calclum = NULL;//for Luminance denoise curve
        NoiseCurve noiseLCurve;
        NoiseCurve noiseCCurve;
        float autoNR = (float) settings->nrauto;//
        float autoNRmax = (float) settings->nrautomax;//

        params.dirpyrDenoise.getCurves(noiseLCurve, noiseCCurve);

        int tilesize;
        int overlap;

        if(settings->leveldnti == 0) {
            tilesize = 1024;
            overlap = 128;
        }

        if(settings->leveldnti == 1) {
            tilesize = 768;
            overlap = 96;
        }

        int numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip;
        int kall = 2;

        parent->ipf.Tile_calc (tilesize, overlap, kall, widIm, heiIm, numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip);
        kall = 0;

        float *ch_M = new float [9];//allocate memory
        float *max_r = new float [9];
        float *max_b = new float [9];
        float *min_b = new float [9];
        float *min_r = new float [9];
        float *lumL = new float [9];
        float *chromC = new float [9];
        float *ry = new float [9];
        float *sk = new float [9];
        float *pcsk = new float [9];
        int *centerTile_X = new int [numtiles_W];
        int *centerTile_Y = new int [numtiles_H];

        for(int cX = 0; cX < numtiles_W; cX++) {
            centerTile_X[cX] = tileWskip / 2 + tileWskip * cX;
        }

        for(int cY = 0; cY < numtiles_H; cY++) {
            centerTile_Y[cY] = tileHskip / 2 + tileHskip * cY;
        }

        if(settings->leveldnautsimpl == 1) {
            if(params.dirpyrDenoise.Cmethod == "MAN" || params.dirpyrDenoise.Cmethod == "PON" )  {
                PreviewProps pp (trafx, trafy, trafw * skip, trafh * skip, skip);
                parent->imgsrc->getImage (parent->currWB, tr, origCrop, pp, params.toneCurve, params.icm, params.raw );
            }
        } else {
            if(params.dirpyrDenoise.C2method == "MANU")  {
                PreviewProps pp (trafx, trafy, trafw * skip, trafh * skip, skip);
                parent->imgsrc->getImage (parent->currWB, tr, origCrop, pp, params.toneCurve, params.icm, params.raw );
            }
        }

        if((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "PRE") || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "PREV")) {
            PreviewProps pp (trafx, trafy, trafw * skip, trafh * skip, skip);
            parent->imgsrc->getImage (parent->currWB, tr, origCrop, pp, params.toneCurve, params.icm, params.raw );

            if((!isDetailWindow) && parent->adnListener && skip == 1 && params.dirpyrDenoise.enabled) {
                float lowdenoise = 1.f;
                int levaut = settings->leveldnaut;

                if(levaut == 1) { //Standard
                    lowdenoise = 0.7f;
                }

                int CenterPreview_X = trafx + (trafw * skip) / 2;
                int CenterPreview_Y = trafy + (trafh * skip) / 2;
                int minimuX = 20000;
                int minimuY = 20000;
                int poscenterX = 0;
                int poscenterY = 0;

                for(int cc = 0; cc < numtiles_W; cc++) {
                    if(abs(centerTile_X[cc] - CenterPreview_X) < minimuX) {
                        minimuX = abs(centerTile_X[cc] - CenterPreview_X);
                        poscenterX = cc;
                    }
                }

                for(int cc = 0; cc < numtiles_H; cc++) {
                    if(abs(centerTile_Y[cc] - CenterPreview_Y) < minimuY) {
                        minimuY = abs(centerTile_Y[cc] - CenterPreview_Y);
                        poscenterY = cc;
                    }
                }

                //  printf("TileCX=%d  TileCY=%d  prevX=%d  prevY=%d \n",centerTile_X[poscenterX],centerTile_Y[poscenterY],CenterPreview_X,CenterPreview_Y);
                int crW;

                if(settings->leveldnv == 0) {
                    crW = 100;
                }

                if(settings->leveldnv == 1) {
                    crW = 250;
                }

                //  if(settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int((tileWskip/2));}//adapted to scale of preview
                if(settings->leveldnv == 2) {
                    crW = int(tileWskip / 2);
                }

                if(settings->leveldnv == 3) {
                    crW = tileWskip - 10;
                }

                float adjustr = 1.f;

                if      (params.icm.working == "ProPhoto")   {
                    adjustr = 1.f;
                } else if (params.icm.working == "Adobe RGB")  {
                    adjustr = 1.f / 1.3f;
                } else if (params.icm.working == "sRGB")       {
                    adjustr = 1.f / 1.3f;
                } else if (params.icm.working == "WideGamut")  {
                    adjustr = 1.f / 1.1f;
                } else if (params.icm.working == "Beta RGB")   {
                    adjustr = 1.f / 1.2f;
                } else if (params.icm.working == "BestRGB")    {
                    adjustr = 1.f / 1.2f;
                } else if (params.icm.working == "BruceRGB")   {
                    adjustr = 1.f / 1.2f;
                }

                if(parent->adnListener) {
                    parent->adnListener->noiseTilePrev (centerTile_X[poscenterX], centerTile_Y[poscenterY], CenterPreview_X, CenterPreview_Y, crW, trafw * skip);
                }

                // I have tried "blind" some solutions..to move review ...but GUI is not my truc !
                //  int W,H;
                //  cropgl->cropMoved (centerTile_X[poscenterX],centerTile_Y[poscenterY] , W, H);
                //   cropImageListener->setPosition (int x, int y, bool update=true);
                //   bool update;
                //   cropImageListener->setPosition (centerTile_X[poscenterX],centerTile_Y[poscenterY] , true);
                //setCropSizes (centerTile_X[poscenterX], centerTile_Y[poscenterY], trafw*skip,trafh*skip , skip, true);

                // we only need image reduced to 1/4 here
                int W = origCrop->getWidth();
                int H = origCrop->getHeight();
                Imagefloat *provicalc = new Imagefloat ((W + 1) / 2, (H + 1) / 2); //for denoise curves

                for(int ii = 0; ii < H; ii += 2) {
                    for(int jj = 0; jj < W; jj += 2) {
                        provicalc->r(ii >> 1, jj >> 1) = origCrop->r(ii, jj);
                        provicalc->g(ii >> 1, jj >> 1) = origCrop->g(ii, jj);
                        provicalc->b(ii >> 1, jj >> 1) = origCrop->b(ii, jj);
                    }
                }

                parent->imgsrc->convertColorSpace(provicalc, params.icm, parent->currWB);//for denoise luminance curve

                float maxr = 0.f;
                float maxb = 0.f;
                float chaut, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, nresi, highresi, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc;
                int Nb;

                chaut = 0.f;
                redaut = 0.f;
                blueaut = 0.f;
                maxredaut = 0.f;
                maxblueaut = 0.f;
                minredaut = 0.f;
                minblueaut = 0.f;
                LUTf gamcurve(65536, 0);
                float gam, gamthresh, gamslope;
                parent->ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
                parent->ipf.RGB_denoise_info(origCrop, provicalc, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope, params.dirpyrDenoise, parent->imgsrc->getDirPyrDenoiseExpComp(), chaut, Nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, nresi, highresi, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc, true);
//                  printf("redy=%f skin=%f pcskin=%f\n",redyel, skinc,nsknc);
//                  printf("DCROP skip=%d cha=%4.0f Nb=%d red=%4.0f bl=%4.0f redM=%4.0f bluM=%4.0f  L=%4.0f sigL=%4.0f Ch=%4.0f Si=%4.0f\n",skip, chaut,Nb, redaut,blueaut, maxredaut, maxblueaut, lumema, sigma_L, chromina, sigma);
                float multip = 1.f;

                if(!parent->imgsrc->isRAW()) {
                    multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good for gamma=1
                }

                float maxmax = max(maxredaut, maxblueaut);
                float delta;
                int mode = 0;
                //  float redyel, skinc, nsknc;
                int lissage = settings->leveldnliss;
                parent->ipf.calcautodn_info (chaut, delta, Nb, levaut, maxmax, lumema, chromina, mode, lissage, redyel, skinc, nsknc);


                if(maxredaut > maxblueaut) {
                    //  maxr=(maxredaut-chaut)/((autoNRmax*multip*adjustr)/2.f);
                    maxr = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                    if(minblueaut <= minredaut  && minblueaut < chaut) {
                        maxb = (-chaut + minblueaut) / (autoNRmax * multip * adjustr * lowdenoise);
                    }
                } else {
                    //  maxb=(maxblueaut-chaut)/((autoNRmax*multip*adjustr)/2.f);
                    maxb = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                    if(minredaut <= minblueaut  && minredaut < chaut) {
                        maxr = (-chaut + minredaut) / (autoNRmax * multip * adjustr * lowdenoise);
                    }
                }//maxb mxr - empirical evaluation red / blue


                params.dirpyrDenoise.chroma = chaut / (autoNR * multip * adjustr * lowdenoise);
                params.dirpyrDenoise.redchro = maxr;
                params.dirpyrDenoise.bluechro = maxb;
                parent->adnListener->chromaChanged(params.dirpyrDenoise.chroma, params.dirpyrDenoise.redchro, params.dirpyrDenoise.bluechro);

                delete provicalc;
            }
        }

        if(skip == 1 && params.dirpyrDenoise.enabled && ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT")  || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO"))) {
            MyTime t1aue, t2aue;
            t1aue.set();

            int crW, crH;

            if(settings->leveldnv == 0) {
                crW = 100;
                crH = 100;
            }

            if(settings->leveldnv == 1) {
                crW = 250;
                crH = 250;
            }

            //  if(settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int((tileWskip/2));}//adapted to scale of preview
            if(settings->leveldnv == 2) {
                crW = int(tileWskip / 2);
                crH = int(tileHskip / 2);
            }

            if(settings->leveldnv == 3) {
                crW = tileWskip - 10;
                crH = tileHskip - 10;
            }

            float lowdenoise = 1.f;
            int levaut = settings->leveldnaut;

            if(levaut == 1) { //Standard
                lowdenoise = 0.7f;
            }

            LUTf gamcurve(65536, 0);
            float gam, gamthresh, gamslope;
            parent->ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
            int Nb[9];
#ifdef _OPENMP
            #pragma omp parallel
#endif
            {
                Imagefloat *origCropPart = new Imagefloat (crW, crH);//allocate memory
                Imagefloat *provicalc = new Imagefloat ((crW + 1) / 2, (crH + 1) / 2); //for denoise curves

                int  coordW[3];//coordonate of part of image to mesure noise
                int  coordH[3];
                int begW = 50;
                int begH = 50;
                coordW[0] = begW;
                coordW[1] = widIm / 2 - crW / 2;
                coordW[2] = widIm - crW - begW;
                coordH[0] = begH;
                coordH[1] = heiIm / 2 - crH / 2;
                coordH[2] = heiIm - crH - begH;
#ifdef _OPENMP
                #pragma omp for schedule(dynamic) collapse(2) nowait
#endif

                for(int wcr = 0; wcr <= 2; wcr++) {
                    for(int hcr = 0; hcr <= 2; hcr++) {
                        PreviewProps ppP (coordW[wcr] , coordH[hcr], crW, crH, 1);
                        parent->imgsrc->getImage (parent->currWB, tr, origCropPart, ppP, params.toneCurve, params.icm, params.raw );

                        // we only need image reduced to 1/4 here
                        for(int ii = 0; ii < crH; ii += 2) {
                            for(int jj = 0; jj < crW; jj += 2) {
                                provicalc->r(ii >> 1, jj >> 1) = origCropPart->r(ii, jj);
                                provicalc->g(ii >> 1, jj >> 1) = origCropPart->g(ii, jj);
                                provicalc->b(ii >> 1, jj >> 1) = origCropPart->b(ii, jj);
                            }
                        }

                        parent->imgsrc->convertColorSpace(provicalc, params.icm, parent->currWB);//for denoise luminance curve

                        float pondcorrec = 1.0f;
                        float chaut = 0.f, redaut = 0.f, blueaut = 0.f, maxredaut = 0.f, maxblueaut = 0.f, minredaut = 0.f, minblueaut = 0.f, nresi = 0.f, highresi = 0.f, chromina = 0.f, sigma = 0.f, lumema = 0.f, sigma_L = 0.f, redyel = 0.f, skinc = 0.f, nsknc = 0.f;
                        int nb = 0;
                        parent->ipf.RGB_denoise_info(origCropPart, provicalc, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope, params.dirpyrDenoise, parent->imgsrc->getDirPyrDenoiseExpComp(), chaut, nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, nresi, highresi, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc);

                        //printf("DCROP skip=%d cha=%f red=%f bl=%f redM=%f bluM=%f chrom=%f sigm=%f lum=%f\n",skip, chaut,redaut,blueaut, maxredaut, maxblueaut, chromina, sigma, lumema);
                        Nb[hcr * 3 + wcr] = nb;
                        ch_M[hcr * 3 + wcr] = pondcorrec * chaut;
                        max_r[hcr * 3 + wcr] = pondcorrec * maxredaut;
                        max_b[hcr * 3 + wcr] = pondcorrec * maxblueaut;
                        min_r[hcr * 3 + wcr] = pondcorrec * minredaut;
                        min_b[hcr * 3 + wcr] = pondcorrec * minblueaut;
                        lumL[hcr * 3 + wcr] = lumema;
                        chromC[hcr * 3 + wcr] = chromina;
                        ry[hcr * 3 + wcr] = redyel;
                        sk[hcr * 3 + wcr] = skinc;
                        pcsk[hcr * 3 + wcr] = nsknc;

                    }
                }

                delete provicalc;
                delete origCropPart;
            }
            float chM = 0.f;
            float MaxR = 0.f;
            float MaxB = 0.f;
            float MinR = 100000000000.f;
            float MinB = 100000000000.f;
            float maxr = 0.f;
            float maxb = 0.f;
            float Max_R[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
            float Max_B[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
            float Min_R[9];
            float Min_B[9];
            float MaxRMoy = 0.f;
            float MaxBMoy = 0.f;
            float MinRMoy = 0.f;
            float MinBMoy = 0.f;

            float multip = 1.f;

            if(!parent->imgsrc->isRAW()) {
                multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good fot gamma=1
            }

            float adjustr = 1.f;

            if      (params.icm.working == "ProPhoto")   {
                adjustr = 1.f;   //
            } else if (params.icm.working == "Adobe RGB")  {
                adjustr = 1.f / 1.3f;
            } else if (params.icm.working == "sRGB")       {
                adjustr = 1.f / 1.3f;
            } else if (params.icm.working == "WideGamut")  {
                adjustr = 1.f / 1.1f;
            } else if (params.icm.working == "Beta RGB")   {
                adjustr = 1.f / 1.2f;
            } else if (params.icm.working == "BestRGB")    {
                adjustr = 1.f / 1.2f;
            } else if (params.icm.working == "BruceRGB")   {
                adjustr = 1.f / 1.2f;
            }

            float delta[9];
            int mode = 1;
            int lissage = settings->leveldnliss;

            for (int k = 0; k < 9; k++) {
                float maxmax = max(max_r[k], max_b[k]);
                parent->ipf.calcautodn_info (ch_M[k], delta[k], Nb[k], levaut, maxmax, lumL[k], chromC[k], mode, lissage, ry[k], sk[k], pcsk[k]);
                //  printf("ch_M=%f delta=%f\n",ch_M[k], delta[k]);
            }

            for (int k = 0; k < 9; k++) {
                if(max_r[k] > max_b[k]) {
                    Max_R[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                    Min_B[k] = -(ch_M[k] - min_b[k]) / (autoNRmax * multip * adjustr * lowdenoise);
                    Max_B[k] = 0.f;
                    Min_R[k] = 0.f;
                } else {
                    Max_B[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                    Min_R[k] = - (ch_M[k] - min_r[k])   / (autoNRmax * multip * adjustr * lowdenoise);
                    Min_B[k] = 0.f;
                    Max_R[k] = 0.f;
                }
            }

            for (int k = 0; k < 9; k++) {
                //  printf("ch_M= %f Max_R=%f Max_B=%f min_r=%f min_b=%f\n",ch_M[k],Max_R[k], Max_B[k],Min_R[k], Min_B[k]);
                chM += ch_M[k];
                MaxBMoy += Max_B[k];
                MaxRMoy += Max_R[k];
                MinRMoy += Min_R[k];
                MinBMoy += Min_B[k];

                if(Max_R[k] > MaxR) {
                    MaxR = Max_R[k];
                }

                if(Max_B[k] > MaxB) {
                    MaxB = Max_B[k];
                }

                if(Min_R[k] < MinR) {
                    MinR = Min_R[k];
                }

                if(Min_B[k] < MinB) {
                    MinB = Min_B[k];
                }
            }

            chM /= 9;
            MaxBMoy /= 9;
            MaxRMoy /= 9;
            MinBMoy /= 9;
            MinRMoy /= 9;

            if(MaxR > MaxB) {
                maxr = MaxRMoy + (MaxR - MaxRMoy) * 0.66f; //#std Dev
                //maxb=MinB;
                maxb = MinBMoy + (MinB - MinBMoy) * 0.66f;
            } else {
                maxb = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;
                maxr = MinRMoy + (MinR - MinRMoy) * 0.66f;
            }

//                  printf("DCROP skip=%d cha=%f red=%f bl=%f \n",skip, chM,maxr,maxb);
            params.dirpyrDenoise.chroma = chM / (autoNR * multip * adjustr);
            params.dirpyrDenoise.redchro = maxr;
            params.dirpyrDenoise.bluechro = maxb;

            if(parent->adnListener) {
                parent->adnListener->chromaChanged(params.dirpyrDenoise.chroma, params.dirpyrDenoise.redchro, params.dirpyrDenoise.bluechro);
            }

            if (settings->verbose) {
                t2aue.set();
                printf("Info denoise auto performed in %d usec:\n", t2aue.etime(t1aue));
            }

            //end evaluate noise
        }

        //  if(params.dirpyrDenoise.Cmethod=="AUT" || params.dirpyrDenoise.Cmethod=="PON") {//reinit origCrop after Auto
        if((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT")  || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO")) { //reinit origCrop after Auto
            PreviewProps pp (trafx, trafy, trafw * skip, trafh * skip, skip);
            parent->imgsrc->getImage (parent->currWB, tr, origCrop, pp, params.toneCurve, params.icm, params.raw );
        }

        DirPyrDenoiseParams denoiseParams = params.dirpyrDenoise;

        if(params.dirpyrDenoise.Lmethod == "CUR") {
            if(noiseLCurve) {
                denoiseParams.luma = 0.5f;    //very small value to init process - select curve or slider
            } else {
                denoiseParams.luma = 0.0f;
            }
        } else if(denoiseParams.Lmethod == "SLI") {
            noiseLCurve.Reset();
        }

        if((noiseLCurve || noiseCCurve ) && skip == 1 && denoiseParams.enabled)   { //only allocate memory if enabled and skip
            // we only need image reduced to 1/4 here
            int W = origCrop->getWidth();
            int H = origCrop->getHeight();
            calclum = new Imagefloat ((W + 1) / 2, (H + 1) / 2); //for denoise curves

            for(int ii = 0; ii < H; ii += 2) {
                for(int jj = 0; jj < W; jj += 2) {
                    calclum->r(ii >> 1, jj >> 1) = origCrop->r(ii, jj);
                    calclum->g(ii >> 1, jj >> 1) = origCrop->g(ii, jj);
                    calclum->b(ii >> 1, jj >> 1) = origCrop->b(ii, jj);
                }
            }

            parent->imgsrc->convertColorSpace(calclum, params.icm, parent->currWB);//for denoise luminance curve
        }

        if(skip != 1) if(parent->adnListener) {
                parent->adnListener->noiseChanged(0.f, 0.f);
            }

        if (todo & M_LINDENOISE) {
            if (skip == 1 && denoiseParams.enabled) {
                int kall = 0;

                float chaut, redaut, blueaut, maxredaut, maxblueaut, nresi, highresi;
                parent->ipf.RGB_denoise(kall, origCrop, origCrop, calclum, ch_M, max_r, max_b, parent->imgsrc->isRAW(), /*Roffset,*/ denoiseParams, parent->imgsrc->getDirPyrDenoiseExpComp(), noiseLCurve, noiseCCurve, chaut, redaut, blueaut, maxredaut, maxblueaut, nresi, highresi);

                if (parent->adnListener) {
                    parent->adnListener->noiseChanged(nresi, highresi);
                }

                if (settings->leveldnautsimpl == 1) {
                    if ((denoiseParams.Cmethod == "AUT" || denoiseParams.Cmethod == "PRE") && (parent->adnListener)) { // force display value of sliders
                        parent->adnListener->chromaChanged(denoiseParams.chroma, denoiseParams.redchro, denoiseParams.bluechro);
                    }
                } else {
                    if((denoiseParams.C2method == "AUTO" || denoiseParams.C2method == "PREV") && (parent->adnListener)) { // force display value of sliders
                        parent->adnListener->chromaChanged(denoiseParams.chroma, denoiseParams.redchro, denoiseParams.bluechro);
                    }
                }

            }
        }

        parent->imgsrc->convertColorSpace(origCrop, params.icm, parent->currWB);

        delete [] ch_M;
        delete [] max_r;
        delete [] max_b;
        delete [] min_r;
        delete [] min_b;
        delete [] lumL;
        delete [] chromC;
        delete [] ry;
        delete [] sk;
        delete [] pcsk;
        delete [] centerTile_X;
        delete [] centerTile_Y;

    }

    // has to be called after setCropSizes! Tools prior to this point can't handle the Edit mechanism, but that shouldn't be a problem.
    createBuffer(cropw, croph);

    // transform
    if (needstransform) {
        if (!transCrop) {
            transCrop = new Imagefloat (cropw, croph);
        }

        if ((todo & M_TRANSFORM) && needstransform)
            parent->ipf.transform (baseCrop, transCrop, cropx / skip, cropy / skip, trafx / skip, trafy / skip, SKIPS(parent->fw, skip), SKIPS(parent->fh, skip), parent->getFullWidth(), parent->getFullHeight(),
                                   parent->imgsrc->getMetaData()->getFocalLen(), parent->imgsrc->getMetaData()->getFocalLen35mm(),
                                   parent->imgsrc->getMetaData()->getFocusDist(), parent->imgsrc->getRotateDegree(), false);

        if (transCrop) {
            baseCrop = transCrop;
        }
    } else {
        if (transCrop) {
            delete transCrop;
        }

        transCrop = NULL;
    }

    if ((todo & (M_TRANSFORM))  && params.dirpyrequalizer.cbdlMethod == "bef" && params.dirpyrequalizer.enabled && !params.colorappearance.enabled) {

        const int W = baseCrop->getWidth();
        const int H = baseCrop->getHeight();
        LabImage labcbdl(W, H);
        parent->ipf.rgb2lab(*baseCrop, labcbdl, params.icm.working);
        parent->ipf.dirpyrequalizer (&labcbdl, skip);
        parent->ipf.lab2rgb(labcbdl, *baseCrop, params.icm.working);

    }

    // blurmap for shadow & highlights
    if ((todo & M_BLURMAP) && params.sh.enabled) {
        double radius = sqrt (double(SKIPS(parent->fw, skip) * SKIPS(parent->fw, skip) + SKIPS(parent->fh, skip) * SKIPS(parent->fh, skip))) / 2.0;
        double shradius = params.sh.radius;

        if (!params.sh.hq) {
            shradius *= radius / 1800.0;
        }

        if(!cshmap) {
            cshmap = new SHMap (cropw, croph, true);
        }

        cshmap->update (baseCrop, shradius, parent->ipf.lumimul, params.sh.hq, skip);

        if(parent->shmap->min_f < 65535.f) { // don't call forceStat with wrong values
            cshmap->forceStat (parent->shmap->max_f, parent->shmap->min_f, parent->shmap->avg);
        }
    }


    // shadows & highlights & tone curve & convert to cielab
    /*int xref,yref;
    xref=000;yref=000;
    if (colortest && cropw>115 && croph>115)
        for(int j=1;j<5;j++){
            xref+=j*30;yref+=j*30;
            if (settings->verbose) printf("before rgbProc RGB Xr%i Yr%i Skip=%d  R=%f  G=%f  B=%f gamma=%f  \n",xref,yref,skip,
                   baseCrop->r[(int)(xref/skip)][(int)(yref/skip)]/256,
                   baseCrop->g[(int)(xref/skip)][(int)(yref/skip)]/256,
                   baseCrop->b[(int)(xref/skip)][(int)(yref/skip)]/256,
                   parent->imgsrc->getGamma());
        }*/
    float satLimit = float(params.colorToning.satProtectionThreshold) / 100.f * 0.7f + 0.3f;
    float satLimitOpacity = 1.f - (float(params.colorToning.saturatedOpacity) / 100.f);

    if(params.colorToning.enabled  && params.colorToning.autosat) { //for colortoning evaluation of saturation settings
        float moyS = 0.f;
        float eqty = 0.f;
        parent->ipf.moyeqt (baseCrop, moyS, eqty);//return image : mean saturation and standard dev of saturation
        //printf("moy=%f ET=%f\n", moyS,eqty);
        float satp = ((moyS + 1.5f * eqty) - 0.3f) / 0.7f; //1.5 sigma ==> 93% pixels with high saturation -0.3 / 0.7 convert to Hombre scale

        if(satp >= 0.92f) {
            satp = 0.92f;    //avoid values too high (out of gamut)
        }

        if(satp <= 0.15f) {
            satp = 0.15f;    //avoid too low values
        }

        satLimit = 100.f * satp;
        satLimitOpacity = 100.f * (moyS - 0.85f * eqty); //-0.85 sigma==>20% pixels with low saturation
    }

    if (todo & M_RGBCURVE) {
        double rrm, ggm, bbm;
        DCPProfile *dcpProf = parent->imgsrc->getDCP(params.icm, parent->currWB);
        parent->ipf.rgbProc (baseCrop, laboCrop, this, parent->hltonecurve, parent->shtonecurve, parent->tonecurve, cshmap,
                             params.toneCurve.saturation, parent->rCurve, parent->gCurve, parent->bCurve, satLimit , satLimitOpacity, parent->ctColorCurve, parent->ctOpacityCurve, parent->opautili, parent->clToningcurve, parent->cl2Toningcurve,
                             parent->customToneCurve1, parent->customToneCurve2, parent->beforeToneCurveBW, parent->afterToneCurveBW, rrm, ggm, bbm,
                             parent->bwAutoR, parent->bwAutoG, parent->bwAutoB, dcpProf);
    }

    /*xref=000;yref=000;
    if (colortest && cropw>115 && croph>115)
    for(int j=1;j<5;j++){
        xref+=j*30;yref+=j*30;
        if (settings->verbose) {
            printf("after rgbProc RGB Xr%i Yr%i Skip=%d  R=%f  G=%f  B=%f  \n",xref,yref,skip,
                   baseCrop->r[(int)(xref/skip)][(int)(yref/skip)]/256,
                   baseCrop->g[(int)(xref/skip)][(int)(yref/skip)]/256,
                   baseCrop->b[(int)(xref/skip)][(int)(yref/skip)]/256);
            printf("after rgbProc Lab Xr%i Yr%i Skip=%d  l=%f  a=%f  b=%f  \n",xref,yref,skip,
                   laboCrop->L[(int)(xref/skip)][(int)(yref/skip)]/327,
                   laboCrop->a[(int)(xref/skip)][(int)(yref/skip)]/327,
                   laboCrop->b[(int)(xref/skip)][(int)(yref/skip)]/327);
        }
    }*/

    // apply luminance operations
    if (todo & (M_LUMINANCE + M_COLOR)) {
        //I made a little change here. Rather than have luminanceCurve (and others) use in/out lab images, we can do more if we copy right here.
        labnCrop->CopyFrom(laboCrop);


        //parent->ipf.luminanceCurve (labnCrop, labnCrop, parent->lumacurve);
        bool utili = parent->utili;
        bool autili = parent->autili;
        bool butili = parent->butili;
        bool ccutili = parent->ccutili;
        bool clcutili = parent->clcutili;
        bool cclutili = parent->cclutili;
        bool wavcontlutili = parent->wavcontlutili;

        LUTu dummy;
        int moderetinex;
        //    parent->ipf.MSR(labnCrop, labnCrop->W, labnCrop->H, 1);
        parent->ipf.chromiLuminanceCurve (this, 1, labnCrop, labnCrop, parent->chroma_acurve, parent->chroma_bcurve, parent->satcurve, parent->lhskcurve,  parent->clcurve, parent->lumacurve, utili, autili, butili, ccutili, cclutili, clcutili, dummy, dummy);
        parent->ipf.vibrance (labnCrop);

        if((params.colorappearance.enabled && !params.colorappearance.tonecie) ||  (!params.colorappearance.enabled)) {
            parent->ipf.EPDToneMap(labnCrop, 5, 1);
        }

        //parent->ipf.EPDToneMap(labnCrop, 5, 1);    //Go with much fewer than normal iterates for fast redisplay.
        // for all treatments Defringe, Sharpening, Contrast detail , Microcontrast they are activated if "CIECAM" function are disabled
        if (skip == 1) {
            if((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled)) {
                parent->ipf.impulsedenoise (labnCrop);
            }

            if((params.colorappearance.enabled && !settings->autocielab) || (!params.colorappearance.enabled) ) {
                parent->ipf.defringe (labnCrop);
            }

            parent->ipf.MLsharpen (labnCrop);

            if((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled)) {
                parent->ipf.MLmicrocontrast (labnCrop);
                parent->ipf.sharpening (labnCrop, (float**)cbuffer, params.sharpening);
            }
        }

        //   if (skip==1) {
        WaveletParams WaveParams = params.wavelet;

        if(params.dirpyrequalizer.cbdlMethod == "aft") {
            if(((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled))) {
                parent->ipf.dirpyrequalizer (labnCrop, skip);
                //  parent->ipf.Lanczoslab (labnCrop,labnCrop , 1.f/skip);
            }
        }

        int kall = 0;
        int minwin = min(labnCrop->W, labnCrop->H);
        int maxlevelcrop = 10;

        //  if(cp.mul[9]!=0)maxlevelcrop=10;
        // adap maximum level wavelet to size of crop
        if(minwin * skip < 1024) {
            maxlevelcrop = 9;    //sampling wavelet 512
        }

        if(minwin * skip < 512) {
            maxlevelcrop = 8;    //sampling wavelet 256
        }

        if(minwin * skip < 256) {
            maxlevelcrop = 7;    //sampling 128
        }

        if(minwin * skip < 128) {
            maxlevelcrop = 6;
        }

        if(minwin < 64) {
            maxlevelcrop = 5;
        }

        int realtile;

        if(params.wavelet.Tilesmethod == "big") {
            realtile = 22;
        }

        if(params.wavelet.Tilesmethod == "lit") {
            realtile = 12;
        }

        int tilesize;
        int overlap;
        tilesize = 1024;
        overlap = 128;
        tilesize = 128 * realtile;
        //overlap=(int) tilesize*params->wavelet.overl;
        overlap = (int) tilesize * 0.125f;
        //  printf("overl=%d\n",overlap);
        int numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip;

        parent->ipf.Tile_calc (tilesize, overlap, kall, labnCrop->W, labnCrop->H, numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip);
        //now we have tile dimensions, overlaps
        //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        int minsizetile = min(tilewidth, tileheight);
        int maxlev2 = 10;

        if(minsizetile < 1024 && maxlevelcrop == 10) {
            maxlev2 = 9;
        }

        if(minsizetile < 512) {
            maxlev2 = 8;
        }

        if(minsizetile < 256) {
            maxlev2 = 7;
        }

        if(minsizetile < 128) {
            maxlev2 = 6;
        }

        int maxL = min(maxlev2, maxlevelcrop);

        if(parent->awavListener) {
            parent->awavListener->wavChanged(float(maxL));
        }


        //merge images
        //common datas
        LabImage * mergelabpart;
        LabImage * cropmergelab;
        LabImage * mergelab;
        int pos = 0;
        float maxx;
        struct E {
            int W, H, sk;
        } e;
        int disp = 0;
        int newsizH, newsizW;
        int deltapix = 0;
        int wat_hdr = 0;
        int Lwa, Hwa;
        bool sav = (params.wavelet.mergMethod == "savwat" || params.wavelet.mergMethod == "savhdr" || params.wavelet.mergMethod == "savzero");
        bool zero = (params.wavelet.mergMethod == "loadzero"  || params.wavelet.mergMethod == "loadzerohdr");
        bool zerono = (params.wavelet.mergMethod != "loadzero"  && params.wavelet.mergMethod != "loadzerohdr");

        if(params.wavelet.mergMethod == "load" || zero) {
            sav = false;
        }

        //  if(sav) printf("save\n");else printf("autre\n");
        if(params.wavelet.expmerge && sav == false ) { //params.wavelet.mergevMethod != "save") {
            bool toto = true;
            bool merguez = false;
            Glib::ustring  inpu;
            inpu = params.wavelet.inpute;
            //printf("fichier=%s\n", inpu.c_str());
            inpu = inpu.substr (5);
            ofstream fout;
            ifstream fin;
            pos = inpu.find("mer");

            if(pos > 2) {//open file Lab datas with its own size
                MyMutex* merMutex = NULL;
                merMutex = new MyMutex;
                merMutex->lock ();

                fin.open(inpu.c_str(), ios::binary);
                fin.read(reinterpret_cast<char *>(&e), sizeof(e));
                // printf("DWcrop mergelabpart=%d DH=%d\n", e.W, e.H);
                newsizH = e.H;
                newsizW = e.W;
                wat_hdr = e.sk;
                mergelabpart = new LabImage(newsizW, newsizH);

                for(int ir = 0; ir < e.H; ir++)
                    for(int jr = 0; jr < e.W; jr++) {
                        struct X {
                            float L, a, b, ma;
                        } x;
                        fin.read(reinterpret_cast<char *>(&x), sizeof(x));
                        mergelabpart->L[ir][jr] = x.L;
                        mergelabpart->a[ir][jr] = x.a;
                        mergelabpart->b[ir][jr] = x.b;
                        maxx = x.ma;
                    }

                //    printf("maxx=%f\n", maxx);
                fin.close();
                merMutex->unlock ();
                delete merMutex;

                if(params.wavelet.mergMethod != "loadzerohdr"  || (params.wavelet.mergMethod == "loadzerohdr" && newsizW != widIm)) {
                    mergelab = new LabImage(widIm, heiIm);
                    float LT = 1000.f;
                    float aT = 0.f;
                    float bT = 0.f; //red
#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for(int ir = 0; ir < (heiIm); ir++)
                        for(int jr = 0; jr < (widIm); jr++) {//fill with color
                            mergelab->L[ir][jr] = LT;
                            mergelab->a[ir][jr] = aT;
                            mergelab->b[ir][jr] = bT;

                        }

                    //put  datas mergelab inside megelabtotal
                    float percenthig = (float) params.wavelet.balanhig;
                    float percentleft = (float) params.wavelet.balanleft;

                    if(zero) {
                        percenthig = percentleft = 0.f;
                    }

                    Lwa = newsizW;
                    Hwa = newsizH;

                    if(Lwa >= widIm) {
                        Lwa = widIm;
                    }

                    if(Hwa >= heiIm) {
                        Hwa = heiIm;
                    }

                    int difwM = widIm - Lwa;
                    int difw = (int)((percentleft * difwM) / 100.f);
                    //    printf("widIM=%d eW=%d difwM=%d difw=%d\n",  widIm, e.W, difwM, difw);
                    int difhM = heiIm - Hwa;
                    int difh = (int)((percenthig * difhM) / 100.f);
                    //  printf("dcrop difh + Hwa=%d difw + Lwa=%d\n",difh + Hwa,difw + Lwa);
#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for(int ir = difh ; ir < (difh + Hwa); ir++)
                        for(int jr = difw ; jr < (difw + Lwa); jr++) {//
                            mergelab->L[ir][jr] = mergelabpart->L[ir - difh][jr - difw];
                            mergelab->a[ir][jr] = mergelabpart->a[ir - difh][jr - difw];
                            mergelab->b[ir][jr] = mergelabpart->b[ir - difh][jr - difw];
                        }
                } else if(params.wavelet.mergMethod == "loadzerohdr"  && newsizW == widIm) {
                    //  if(newsizW != widIm) {
                    //     return;    //exit if bad selection
                    //  }

                    mergelab = new LabImage(newsizW, newsizH);
                    float LT = 1000.f;
                    float aT = 0.f;
                    float bT = 0.f; //red
                    Lwa = newsizW;
                    Hwa = newsizH;

#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for(int ir = 0; ir < (newsizH); ir++)
                        for(int jr = 0; jr < (newsizW); jr++) {//fill with color
                            mergelab->L[ir][jr] = LT;
                            mergelab->a[ir][jr] = aT;
                            mergelab->b[ir][jr] = bT;

                        }

                    //put  datas mergelab inside megelabtotal

                    float percenthig = (float) params.wavelet.balanhig;
                    float percentleft = (float) params.wavelet.balanleft;
                    int maxshift = 10;

                    int deltawe = maxshift * (percentleft - 50.f) / 100.f; //if pcwe = 50  deltawe = 0 ; if pcwe = 100  deltawe = 2;if pcwe = 0  deltawe = -2
                    int deltahi = maxshift * (percenthig - 50.f) / 100.f;
                    int absw = abs(deltawe);
                    int absh = abs(deltahi);
                    //  printf("DCROP delE=%d delH=%d absw=%d absh=%d\n", deltawe, deltahi, absw, absh);

                    for(int ir = absh; ir < (newsizH - absh); ir++)
                        for(int jr = absw; jr < (newsizW - absw); jr++) { //new image
                            mergelab->L[ir][jr] = mergelabpart->L[ir - deltahi][jr - deltawe];
                            mergelab->a[ir][jr] = mergelabpart->a[ir - deltahi][jr - deltawe];
                            mergelab->b[ir][jr] = mergelabpart->b[ir - deltahi][jr - deltawe];
                        }
                }

                delete mergelabpart;
            }

        }

        //end load Lab datas for merge


        //begin threatment datas Lab for merge
        if(params.wavelet.expmerge && !sav) { //params.wavelet.mergevMethod != "save") {

            if(pos > 2) {

                bool merguez = true;
                cropmergelab = new LabImage(labnCrop->W, labnCrop->H);

                if(merguez) {//merge images
                    disp = 0;

                    if(params.wavelet.mergevMethod == "first") {//old image for watermark
                        disp = 1;
                    }

                    if(params.wavelet.mergevMethod == "cuno") {//current image no merge
                        disp = 2;
                    }

                    if(disp != 2) {
#ifdef _OPENMP
                        #pragma omp parallel for
#endif

                        for(int ir = 0; ir < (labnCrop->H); ir++)
                            for(int jr = 0; jr < (labnCrop->W); jr++) {//take into account crop
                                int irfull, jrfull;
                                irfull = (ir) * skip + rqcropy - skip * upperBorder;
                                jrfull = (jr) * skip + rqcropx - skip * leftBorder;
                                irfull = LIM(irfull, 0, heiIm - 1);
                                jrfull = LIM(jrfull, 0, widIm - 1);
                                cropmergelab->L[ir][jr] = mergelab->L[irfull][jrfull];
                                cropmergelab->a[ir][jr] = mergelab->a[irfull][jrfull];
                                cropmergelab->b[ir][jr] = mergelab->b[irfull][jrfull];

                                if(disp == 1) {
                                    labnCrop->L[ir][jr] = cropmergelab->L[ir][jr];    //merge 100%
                                    labnCrop->a[ir][jr] = cropmergelab->a[ir][jr];    //merge 100%
                                    labnCrop->b[ir][jr] = cropmergelab->b[ir][jr];    //merge 100%
                                }

                            }
                    }
                }

                delete mergelab;
            }
        }

        //end threatment datas Lab for merge


        int merge_two[6] = {0, 0, 0, 0, 0, 0};
        int mtwo;



        if((params.wavelet.enabled)) {
            WavCurve wavCLVCurve;
            WavretiCurve wavRETCurve;
            WavretigainCurve wavRETgainCurve;
            WavmergCurve wavMERCurve;
            Wavmerg2Curve wavMER2Curve;
            WavstyCurve wavSTYCurve;
            Wavsty2Curve wavSTY2Curve;

            WavOpacityCurveRG waOpacityCurveRG;
            WavOpacityCurveBY waOpacityCurveBY;
            WavOpacityCurveW waOpacityCurveW;
            WavOpacityCurveWL waOpacityCurveWL;
            LUTf wavclCurve;
            LUTu dummy;

            params.wavelet.getCurves(wavCLVCurve, wavRETCurve,  wavRETgainCurve, wavMERCurve, wavMER2Curve, wavSTYCurve, wavSTY2Curve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL);
            LabImage *unshar;
            Glib::ustring provis;
            float minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax;
            minCD = maxCD = maxi = mini = Tmean = Tsigma = Tmin = Tmax = 0.f;
            float *****stylev;
            LabImage *styres;
            int stytype = 0;

            //params.wavelet.mergevMethod == "cuno" // no threatment
            if(params.wavelet.expmerge && params.wavelet.mergevMethod == "curr") { //merge datas for Watermark if not preview old datas

                //   unshar = new LabImage (labnCrop->W, labnCrop->H);
                //   unshar->CopyFrom(labnCrop);

                if(pos > 2) {
                    float mLY, mCY;
                    float m_L = (float) (WaveParams.blend / 100.f);
                    float m_C = (float) (WaveParams.blendc / 100.f);
                    float gra = WaveParams.grad / 150.f;
                    mLY = m_L;
                    mCY = m_C;

                    if(params.wavelet.mergBMethod == "hdr1" && wavMERCurve  && (zerono)) {

                        float Mlc;
#ifdef _OPENMP
                        #pragma omp parallel for
#endif

                        for (int x = 0; x < labnCrop->H; x++)
                            for (int y = 0; y < labnCrop->W; y++) {
                                Mlc = 1.4f * wavMERCurve[cropmergelab->L[x][y] / 65.f] - 0.5f;
                                labnCrop->L[x][y] =  Mlc * (cropmergelab->L[x][y]) + labnCrop->L[x][y];
                                labnCrop->a[x][y] =  m_C * (cropmergelab->a[x][y]) + labnCrop->a[x][y];
                                labnCrop->b[x][y] =  m_C * (cropmergelab->b[x][y]) + labnCrop->b[x][y];
                            }
                    }

                    if(params.wavelet.mergBMethod == "hdr2") {//

                        float Mlv;
#ifdef _OPENMP
                        #pragma omp parallel for
#endif

                        for (int x = 0; x < labnCrop->H; x++)
                            for (int y = 0; y < labnCrop->W; y++) {
                                Mlv = (m_L / 33000.f) * (gra - 1.f) * cropmergelab->L[x][y] + m_L;
                                labnCrop->L[x][y] =  Mlv * (cropmergelab->L[x][y]) + labnCrop->L[x][y];
                                labnCrop->a[x][y] =  m_C * (cropmergelab->a[x][y]) + labnCrop->a[x][y];
                                labnCrop->b[x][y] =  m_C * (cropmergelab->b[x][y]) + labnCrop->b[x][y];
                            }
                    }

                    if(zero) {
                        stytype = 1;
                        int lab = 3;
                        int leve =  params.wavelet.thres;
                        int dir = 3;
                        int hei = (cropmergelab->H) / 2 + 1; //fh / 2;
                        int wid = (cropmergelab->W) / 2 + 1; //fw / 2;

                        merge_two[0] = int( wid * ((float)Lwa / (float)widIm));//
                        merge_two[1] = int( hei * ((float)Hwa / (float)heiIm)) ;//
                        int wid1 = merge_two[0] + 1;
                        int hei1 = merge_two[1] + 1;

                        if(merge_two[0] > wid) {
                            merge_two[0] = wid;
                        }

                        if(merge_two[1] > hei) {
                            merge_two[1] = hei;
                        }

                        //  printf("he=%d wi=%d\n",hei,wid);
                        if(stytype == 1) {
                            stylev = new float****[lab];

                            for(int y = 0; y < lab; y++) {
                                stylev[y] = new float***[dir];

                                for (int d = 0; d < dir; d++) {
                                    stylev[y][d] = new float**[leve];

                                    for (int k = 0; k < leve; k++) {
                                        stylev[y][d][k] = new float*[hei1];

                                        for (int i = 0; i < hei1; i++) {
                                            stylev[y][d][k][i] = new float[wid1];
                                        }
                                    }
                                }
                            }

                            styres = new LabImage(wid1, hei1);

                            parent->ipf.ip_wavelet(cropmergelab, cropmergelab, stylev, styres, stytype, NULL, mtwo, merge_two, 1, kall, WaveParams, wavMER2Curve, wavCLVCurve, wavRETCurve, wavRETgainCurve, wavSTYCurve, wavSTY2Curve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, wavcontlutili, skip, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);
                        }

                        stytype = 2;

                        if(stytype == 2) {

                            parent->ipf.ip_wavelet(labnCrop, labnCrop, stylev, styres, stytype, cropmergelab, mtwo, merge_two, 1, kall, WaveParams, wavMER2Curve, wavCLVCurve, wavRETCurve, wavRETgainCurve, wavSTYCurve, wavSTY2Curve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, wavcontlutili, skip, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);

                            for(int y = 0; y < lab; y++) {
                                for (int i = 0; i < dir; i++) {
                                    for (int j = 0; j < leve; j++) {
                                        for (int l = 0; l < hei1; l++) {
                                            delete [] stylev[y][i][j][l];
                                        }
                                    }
                                }
                            }

                            for(int y = 0; y < lab; y++) {
                                for (int i = 0; i < dir; i++) {
                                    for (int j = 0; j < leve; j++) {
                                        delete [] stylev[y][i][j];
                                    }
                                }
                            }

                            for(int y = 0; y < lab; y++) {
                                for (int i = 0; i < dir; i++) {
                                    delete [] stylev[y][i];
                                }
                            }

                            for(int y = 0; y < lab; y++) {
                                delete [] stylev[y];
                            }

                            delete [] stylev;
                            delete  styres;

                        }
                    }



                    delete cropmergelab;

                }

                //   delete unshar;
                //   unshar    = NULL;

            }





            if(WaveParams.ushamethod != "none" && WaveParams.expedge && WaveParams.CLmethod != "all" && zerono) {

                unshar = new LabImage (labnCrop->W, labnCrop->H);
                provis = params.wavelet.CLmethod;
                params.wavelet.CLmethod = "all";

                parent->ipf.ip_wavelet(labnCrop, labnCrop, stylev, styres, stytype, NULL, mtwo, merge_two, 1, kall, WaveParams, wavMER2Curve, wavCLVCurve, wavRETCurve, wavRETgainCurve, wavSTYCurve, wavSTY2Curve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, wavcontlutili, skip, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);
                unshar->CopyFrom(labnCrop);

                params.wavelet.CLmethod = provis;

            }

            //     if(params.wavelet.mergMethod != "loadzero" || (params.wavelet.mergMethod == "loadzero" &&  !params.wavelet.expmerge)) {
            if(zerono || (zero &&  !params.wavelet.expmerge)) {
                parent->ipf.ip_wavelet(labnCrop, labnCrop, stylev, styres, stytype, NULL, mtwo, merge_two, 0, kall, WaveParams, wavMER2Curve, wavCLVCurve, wavRETCurve, wavRETgainCurve, wavSTYCurve, wavSTY2Curve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, wavcontlutili, skip, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);
            }

            if(parent->awavListener) {

                parent->awavListener->minmaxChanged(maxCD, minCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);

            }

            if(WaveParams.ushamethod != "none"  && WaveParams.expedge && WaveParams.CLmethod != "all" && zerono) {

                float mL = (float) (WaveParams.mergeL / 100.f);
                float mC = (float) (WaveParams.mergeC / 100.f);
                float mL0;
                float mC0;

                if((WaveParams.CLmethod == "one" || WaveParams.CLmethod == "inf")  && WaveParams.Backmethod == "black") {
                    mL0 = mC0 = 0.f;
                    mL = -mL;
                    mC = -mC;
                } else if(WaveParams.CLmethod == "sup" && WaveParams.Backmethod == "resid") {
                    mL0 = mL;
                    mC0 = mC;
                } else {
                    mL0 = mL = mC0 = mC = 0.f;
                }

#ifdef _OPENMP
                #pragma omp parallel for
#endif

                for (int x = 0; x < labnCrop->H; x++)
                    for (int y = 0; y < labnCrop->W; y++) {
                        labnCrop->L[x][y] = (1.f + mL0) * (unshar->L[x][y]) - mL * labnCrop->L[x][y];
                        labnCrop->a[x][y] = (1.f + mC0) * (unshar->a[x][y]) - mC * labnCrop->a[x][y];
                        labnCrop->b[x][y] = (1.f + mC0) * (unshar->b[x][y]) - mC * labnCrop->b[x][y];
                    }

                delete unshar;
                unshar    = NULL;
            }
        }


        if(params.colorappearance.enabled) {
            float fnum = parent->imgsrc->getMetaData()->getFNumber  ();        // F number
            float fiso = parent->imgsrc->getMetaData()->getISOSpeed () ;       // ISO
            float fspeed = parent->imgsrc->getMetaData()->getShutterSpeed () ; // Speed
            double fcomp = parent->imgsrc->getMetaData()->getExpComp  ();      // Compensation +/-
            double adap; // Scene's luminosity adaptation factor

            if(fnum < 0.3f || fiso < 5.f || fspeed < 0.00001f) { //if no exif data or wrong
                adap = 2000.;
            } else {
                double E_V = fcomp + log2 (double((fnum * fnum) / fspeed / (fiso / 100.f)));
                E_V += params.toneCurve.expcomp;// exposure compensation in tonecurve ==> direct EV
                E_V += log2(params.raw.expos);// exposure raw white point ; log2 ==> linear to EV
                adap = pow(2., E_V - 3.); // cd / m2
                // end calculation adaptation scene luminosity
            }

            int begh = 0, endh = labnCrop->H;
            bool execsharp = false;

            if(skip == 1) {
                execsharp = true;
            }

            if (!cieCrop) {
                cieCrop = new CieImage (cropw, croph);
            }

            if(settings->ciecamfloat) {
                float d; // not used after this block
                parent->ipf.ciecam_02float (cieCrop, float(adap), begh, endh, 1, 2, labnCrop, &params, parent->customColCurve1, parent->customColCurve2, parent->customColCurve3,
                                            dummy, dummy, parent->CAMBrightCurveJ, parent->CAMBrightCurveQ, parent->CAMMean, 5, 1, execsharp, d, skip, 1);
            } else {
                double dd; // not used after this block

                parent->ipf.ciecam_02 (cieCrop, adap, begh, endh, 1, 2, labnCrop, &params, parent->customColCurve1, parent->customColCurve2, parent->customColCurve3,
                                       dummy, dummy, parent->CAMBrightCurveJ, parent->CAMBrightCurveQ, parent->CAMMean, 5, 1, execsharp, dd, skip, 1);
            }
        } else {
            // CIECAM is disbaled, we free up its image buffer to save some space
            if (cieCrop) {
                delete cieCrop;
            }

            cieCrop = NULL;
        }
    }

    // all pipette buffer processing should be finished now
    PipetteBuffer::setReady();

    // switch back to rgb
    parent->ipf.lab2monitorRgb (labnCrop, cropImg);

    if (cropImageListener) {
        // this in output space held in parallel to allow analysis like shadow/highlight
        Glib::ustring outProfile = params.icm.output;
        Glib::ustring workProfile = params.icm.working;
        Image8 *cropImgtrue;

        if(settings->HistogramWorking) {
            cropImgtrue = parent->ipf.lab2rgb (labnCrop, 0, 0, cropw, croph, workProfile, RI_RELATIVE, false);  // HOMBRE: was RELATIVE by default in lab2rgb, is it safe to assume we have to use it again ?
        } else {
            if (params.icm.output == "" || params.icm.output == ColorManagementParams::NoICMString) {
                outProfile = "sRGB";
            }

            cropImgtrue = parent->ipf.lab2rgb (labnCrop, 0, 0, cropw, croph, outProfile, params.icm.outputIntent, false);
        }

        int finalW = rqcropw;

        if (cropImg->getWidth() - leftBorder < finalW) {
            finalW = cropImg->getWidth() - leftBorder;
        }

        int finalH = rqcroph;

        if (cropImg->getHeight() - upperBorder < finalH) {
            finalH = cropImg->getHeight() - upperBorder;
        }

        Image8* final = new Image8 (finalW, finalH);
        Image8* finaltrue = new Image8 (finalW, finalH);

        for (int i = 0; i < finalH; i++) {
            memcpy (final->data + 3 * i * finalW, cropImg->data + 3 * (i + upperBorder)*cropw + 3 * leftBorder, 3 * finalW);
            memcpy (finaltrue->data + 3 * i * finalW, cropImgtrue->data + 3 * (i + upperBorder)*cropw + 3 * leftBorder, 3 * finalW);
        }

        cropImageListener->setDetailedCrop (final, finaltrue, params.icm, params.crop, rqcropx, rqcropy, rqcropw, rqcroph, skip);
        delete final;
        delete finaltrue;
        delete cropImgtrue;
    }
}

void Crop::freeAll ()
{

    if (settings->verbose) {
        printf ("freeallcrop starts %d\n", (int)cropAllocated);
    }

    if (cropAllocated) {
        if (origCrop ) {
            delete    origCrop;
            origCrop = NULL;
        }

        if (transCrop) {
            delete    transCrop;
            transCrop = NULL;
        }

        if (laboCrop ) {
            delete    laboCrop;
            laboCrop = NULL;
        }

        if (labnCrop ) {
            delete    labnCrop;
            labnCrop = NULL;
        }

        if (cropImg  ) {
            delete    cropImg;
            cropImg = NULL;
        }

        if (cieCrop  ) {
            delete    cieCrop;
            cieCrop = NULL;
        }

        if (cbuf_real) {
            delete [] cbuf_real;
            cbuf_real = NULL;
        }

        if (cbuffer  ) {
            delete [] cbuffer;
            cbuffer = NULL;
        }

        if (cshmap   ) {
            delete    cshmap;
            cshmap = NULL;
        }

        PipetteBuffer::flush();
    }

    cropAllocated = false;
}

/** @brief Handles crop's image buffer reallocation and trigger sizeChanged of SizeListener[s]
 * If the scale changes, this method will free all buffers and reallocate ones of the new size.
 * It will then tell to the SizeListener that size has changed (sizeChanged)
 */
bool Crop::setCropSizes (int rcx, int rcy, int rcw, int rch, int skip, bool internal)
{

    if (settings->verbose) {
        printf ("setcropsizes before lock\n");
    }

    if (!internal) {
        cropMutex.lock ();
    }

    bool changed = false;

    rqcropx = rcx;
    rqcropy = rcy;
    rqcropw = rcw;
    rqcroph = rch;

    // store and set requested crop size
    int rqx1 = LIM(rqcropx, 0, parent->fullw - 1);
    int rqy1 = LIM(rqcropy, 0, parent->fullh - 1);
    int rqx2 = rqx1 + rqcropw - 1;
    int rqy2 = rqy1 + rqcroph - 1;
    rqx2 = LIM(rqx2, 0, parent->fullw - 1);
    rqy2 = LIM(rqy2, 0, parent->fullh - 1);

    this->skip = skip;

    // add border, if possible
    int bx1 = rqx1 - skip * borderRequested;
    int by1 = rqy1 - skip * borderRequested;
    int bx2 = rqx2 + skip * borderRequested;
    int by2 = rqy2 + skip * borderRequested;
    // clip it to fit into image area
    bx1 = LIM(bx1, 0, parent->fullw - 1);
    by1 = LIM(by1, 0, parent->fullh - 1);
    bx2 = LIM(bx2, 0, parent->fullw - 1);
    by2 = LIM(by2, 0, parent->fullh - 1);
    int bw = bx2 - bx1 + 1;
    int bh = by2 - by1 + 1;

    // determine which part of the source image is required to compute the crop rectangle
    int orx, ory, orw, orh;
    ProcParams& params = parent->params;
    parent->ipf.transCoord (parent->fw, parent->fh, bx1, by1, bw, bh, orx, ory, orw, orh);

    int tr = getCoarseBitMask(params.coarse);

    PreviewProps cp (orx, ory, orw, orh, skip);
    int orW, orH;
    parent->imgsrc->getSize (tr, cp, orW, orH);

    int cw = SKIPS(bw, skip);
    int ch = SKIPS(bh, skip);

    leftBorder  = SKIPS(rqx1 - bx1, skip);
    upperBorder = SKIPS(rqy1 - by1, skip);

    if (settings->verbose) {
        printf ("setsizes starts (%d, %d, %d, %d, %d, %d)\n", orW, orH, trafw, trafh, cw, ch);
    }

    EditType editType = ET_PIPETTE;

    if (const auto editProvider = PipetteBuffer::getDataProvider ()) {
        if (const auto editSubscriber = editProvider->getCurrSubscriber ()) {
            editType = editSubscriber->getEditingType ();
        }
    }

    if (cw != cropw || ch != croph || orW != trafw || orH != trafh) {

        cropw = cw;
        croph = ch;
        trafw = orW;
        trafh = orH;

        if (!origCrop) {
            origCrop = new Imagefloat;
        }

        origCrop->allocate(trafw, trafh); // Resizing the buffer (optimization)

        // if transCrop doesn't exist yet, it'll be created where necessary
        if (transCrop) {
            transCrop->allocate(cropw, croph);
        }

        if (laboCrop) {
            delete laboCrop;    // laboCrop can't be resized
        }

        laboCrop = new LabImage (cropw, croph);

        if (labnCrop) {
            delete labnCrop;    // labnCrop can't be resized
        }

        labnCrop = new LabImage (cropw, croph);

        if (!cropImg) {
            cropImg = new Image8;
        }

        cropImg->allocate(cropw, croph); // Resizing the buffer (optimization)

        //cieCrop is only used in Crop::update, it is destroyed now but will be allocated on first use
        if (cieCrop) {
            delete cieCrop;
            cieCrop = NULL;
        }

        if (cbuffer  ) {
            delete [] cbuffer;
        }

        if (cbuf_real) {
            delete [] cbuf_real;
        }

        if (cshmap   ) {
            delete    cshmap;
            cshmap = NULL;
        }

        cbuffer = new float*[croph];
        cbuf_real = new float[(croph + 2)*cropw];

        for (int i = 0; i < croph; i++) {
            cbuffer[i] = cbuf_real + cropw * i + cropw;
        }

        if(params.sh.enabled) {
            cshmap = new SHMap (cropw, croph, true);
        }

        if (editType == ET_PIPETTE) {
            PipetteBuffer::resize(cropw, croph);
        } else if (PipetteBuffer::bufferCreated()) {
            PipetteBuffer::flush();
        }

        cropAllocated = true;

        changed = true;
    }

    cropx = bx1;
    cropy = by1;
    trafx = orx;
    trafy = ory;

    if (settings->verbose) {
        printf ("setsizes ends\n");
    }

    if (!internal) {
        cropMutex.unlock ();
    }

    return changed;
}

/** @brief Look out if a new thread has to be started to process the update
  *
  * @return If true, a new updating thread has to be created. If false, the current updating thread will be used
  */
bool Crop::tryUpdate()
{
    bool needsNewThread = true;

    if (updating) {
        // tells to the updater thread that a new update is pending
        newUpdatePending = true;
        // no need for a new thread, the current one will do the job
        needsNewThread = false;
    } else
        // the crop is now being updated ...well, when fullUpdate will be called
    {
        updating = true;
    }

    return needsNewThread;
}

/* @brief Handles Crop updating in its own thread
 *
 * This method will cycle updates as long as Crop::newUpdatePending will be true. During the processing,
 * intermediary update will be automatically flushed by Crop::tryUpdate.
 *
 * This method is called when the visible part of the crop has changed (resize, zoom, etc..), so it needs a full update
 */
void Crop::fullUpdate ()
{

    parent->updaterThreadStart.lock ();

    if (parent->updaterRunning && parent->thread) {
        // Do NOT reset changes here, since in a long chain of events it will lead to chroma_scale not being updated,
        // causing Color::lab2rgb to return a black image on some opens
        //parent->changeSinceLast = 0;
        parent->thread->join ();
    }

    if (parent->plistener) {
        parent->plistener->setProgressState (true);
    }

    // If there are more update request, the following WHILE will collect it
    newUpdatePending = true;

    while (newUpdatePending) {
        newUpdatePending = false;
        update (ALL);
    }

    updating = false;  // end of crop update

    if (parent->plistener) {
        parent->plistener->setProgressState (false);
    }

    parent->updaterThreadStart.unlock ();
}

}

