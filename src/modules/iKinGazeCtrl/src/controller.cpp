/* 
 * Copyright (C) 2010 RobotCub Consortium, European Commission FP6 Project IST-004370
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * website: www.robotcub.org
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <iCub/solver.h>
#include <iCub/controller.h>


/************************************************************************/
Controller::Controller(PolyDriver *_drvTorso, PolyDriver *_drvHead, exchangeData *_commData,
                       const string &_robotName, const string &_localName, const string &_configFile,
                       const double _neckTime, const double _eyesTime, const double _eyeTiltMin,
                       const double _eyeTiltMax, const double _minAbsVel, unsigned int _period) :
                       RateThread(_period),     drvTorso(_drvTorso),     drvHead(_drvHead),
                       commData(_commData),     robotName(_robotName),   localName(_localName),
                       configFile(_configFile), neckTime(_neckTime),     eyesTime(_eyesTime),
                       eyeTiltMin(_eyeTiltMin), eyeTiltMax(_eyeTiltMax), minAbsVel(_minAbsVel),
                       period(_period),         Ts(_period/1000.0),      printAccTime(0.0)
{
    Robotable=(drvHead!=NULL);

    // Instantiate objects
    neck=new iCubHeadCenter();
    eyeL=new iCubEye("left");
    eyeR=new iCubEye("right");

    // release links
    neck->releaseLink(0); eyeL->releaseLink(0); eyeR->releaseLink(0);
    neck->releaseLink(1); eyeL->releaseLink(1); eyeR->releaseLink(1);
    neck->releaseLink(2); eyeL->releaseLink(2); eyeR->releaseLink(2);

    // Get the chain objects
    chainNeck=neck->asChain();
    chainEyeL=eyeL->asChain();
    chainEyeR=eyeR->asChain();

    // add aligning links read from configuration file
    if (getAlignLinks(configFile,"ALIGN_KIN_LEFT",&alignLnkLeft1,&alignLnkLeft2))
    {
        *chainEyeL<<*alignLnkLeft1<<*alignLnkLeft2;
        chainEyeL->blockLink(chainEyeL->getN()-1,0.0);
        chainEyeL->blockLink(chainEyeL->getN()-2,0.0);
    }
    else
        alignLnkLeft1=alignLnkLeft2=NULL;

    if (getAlignLinks(configFile,"ALIGN_KIN_RIGHT",&alignLnkRight1,&alignLnkRight2))
    {
        *chainEyeR<<*alignLnkRight1<<*alignLnkRight2;
        chainEyeR->blockLink(chainEyeR->getN()-1,0.0);
        chainEyeR->blockLink(chainEyeR->getN()-2,0.0);
    }
    else
        alignLnkRight1=alignLnkRight2=NULL;

    Matrix lim;

    if (Robotable)
    {
        // create interfaces
        bool ok=true;

        if (drvTorso!=NULL)
            ok&=drvTorso->view(encTorso);
        else
            encTorso=NULL;

        ok&=drvHead->view(encHead);
        ok&=drvHead->view(velHead);

        if (!ok)
            fprintf(stdout,"Problems acquiring interfaces!\n");

        // read number of joints
        if (encTorso!=NULL)
            encTorso->getAxes(&nJointsTorso);
        else
            nJointsTorso=3;

        encHead->getAxes(&nJointsHead);

        // joints bounds alignment
        lim=alignJointsBounds(chainNeck,drvTorso,drvHead,eyeTiltMin,eyeTiltMax);
        copyJointsBounds(chainNeck,chainEyeL);
        copyJointsBounds(chainEyeL,chainEyeR);

        // reinforce vergence min bound
        lim(nJointsHead-1,0)=MINALLOWED_VERGENCE*CTRL_DEG2RAD;

        // read starting position
        fbTorso.resize(nJointsTorso,0.0);
        fbHead.resize(nJointsHead,0.0);
        getFeedback(fbTorso,fbHead,encTorso,encHead);

        // exclude acceleration constraints by fixing
        // thresholds at high values
        Vector a_robHead(nJointsHead); a_robHead=1e9;
        velHead->setRefAccelerations(a_robHead.data());
    }
    else
    {
        nJointsTorso=3;
        nJointsHead =6;
        
        // create bounds matrix for integrators
        lim.resize(nJointsHead,2);
        for (int i=0; i<nJointsHead-1; i++)
        {
            lim(i,0)=(*chainNeck)[nJointsTorso+i].getMin();
            lim(i,1)=(*chainNeck)[nJointsTorso+i].getMax();
        }

        // vergence
        lim(nJointsHead-1,0)=MINALLOWED_VERGENCE*CTRL_DEG2RAD;
        lim(nJointsHead-1,1)=lim(nJointsHead-2,1);

        fbTorso.resize(nJointsTorso,0.0);
        fbHead.resize(nJointsHead,0.0);

        // impose starting vergence != 0.0
        fbHead[5]=MINALLOWED_VERGENCE*CTRL_DEG2RAD;
    }    

    fbNeck.resize(3); fbEyes.resize(3);
    qdNeck.resize(3); qdEyes.resize(3);
    vNeck.resize(3);  vEyes.resize(3);

    // Set the task execution time
    setTeyes(eyesTime);
    setTneck(neckTime);

    mjCtrlNeck=new minJerkVelCtrl(Ts,fbNeck.length());
    mjCtrlEyes=new minJerkVelCtrl(Ts,fbEyes.length());
    Int=new Integrator(Ts,fbHead,lim);
    
    v.resize(nJointsHead,0.0);
    vdegOld=v;

    qd=fbHead;
    qddeg=CTRL_RAD2DEG*qd;
    vdeg =CTRL_RAD2DEG*v;

    commData->get_isCtrlActive()=false;
    commData->get_canCtrlBeDisabled()=true;

    port_xd=NULL;
}


/************************************************************************/
void Controller::stopLimbsVel()
{
    if (Robotable)
    {
        // this timeout prevents the stop() from
        // being overwritten by the last velocityMove()
        // which travels on a different connection.
        Time::delay(2*Ts);

        velHead->stop();
    }
}


/************************************************************************/
void Controller::printIter(Vector &xd, Vector &fp, Vector &qd, Vector &q,
                           Vector &v, double printTime)
{
    if ((printAccTime+=Ts)>=printTime)
    {
        printAccTime=0.0;

        fprintf(stdout,"\n");
        fprintf(stdout,"norm(e)           = %g\n",norm(xd-fp));
        fprintf(stdout,"Target fix. point = %s\n",xd.toString().c_str());
        fprintf(stdout,"Actual fix. point = %s\n",fp.toString().c_str());
        fprintf(stdout,"Target Joints     = %s\n",qd.toString().c_str());
        fprintf(stdout,"Actual Joints     = %s\n",q.toString().c_str());
        fprintf(stdout,"Velocity          = %s\n",v.toString().c_str());
        fprintf(stdout,"\n");
    }
}


/************************************************************************/
bool Controller::threadInit()
{
    port_x.open((localName+"/x:o").c_str());
    port_q.open((localName+"/q:o").c_str());

    fprintf(stdout,"Starting Controller at %d ms\n",period);

    return true;
}


/************************************************************************/
void Controller::afterStart(bool s)
{
    if (s)
        fprintf(stdout,"Controller started successfully\n");
    else
        fprintf(stdout,"Controller did not start\n");
}


/************************************************************************/
void Controller::run()
{
    bool swOffCond=(norm(commData->get_qd()-fbHead)<GAZECTRL_MOTIONDONE_QTHRES*CTRL_DEG2RAD);

    // verify control switching conditions
    if (commData->get_isCtrlActive())
    {
        // switch-off condition
        if (swOffCond)
        {
            stopLimbsVel();

            commData->get_isCtrlActive()=false;
            port_xd->get_new()=false;
        }
    }       
    else if (!swOffCond)
    {
        // switch-on condition
        commData->get_isCtrlActive()=(commData->get_canCtrlBeDisabled() ? 
                                      (port_xd->get_new() || (commData->get_qd()[0]!=qd[0]) || (commData->get_qd()[1]!=qd[1]) || (commData->get_qd()[2]!=qd[2])) :
                                      (norm(port_xd->get_xd()-fp)>GAZECTRL_MOTIONSTART_XTHRES));
    }

    // get data
    xd=commData->get_xd();
    qd=commData->get_qd();
    fp=commData->get_x();

    // Introduce the feedback within the control computation
    if (Robotable)
    {
        if (!getFeedback(fbTorso,fbHead,encTorso,encHead))
        {
            fprintf(stdout,"\nCommunication timeout detected!\n\n");
            suspend();

            return;
        }

        Int->reset(fbHead);
    }
    
    for (unsigned int i=0; i<3; i++)
    {
        qdNeck[i]=qd[i];
        qdEyes[i]=qd[3+i];

        fbNeck[i]=fbHead[i];
        fbEyes[i]=fbHead[3+i];
    }

    if (commData->get_isCtrlActive())
    {
        // control loop
        vNeck=mjCtrlNeck->computeCmd(neckTime,qdNeck-fbNeck);
        vEyes=mjCtrlEyes->computeCmd(eyesTime,qdEyes-fbEyes)+commData->get_counterv();
    }
    else
    {
        vNeck=0.0;
        vEyes=0.0;
    }

    for (unsigned int i=0; i<3; i++)
    {
        v[i]  =vNeck[i];
        v[3+i]=vEyes[i];
    }

    // apply bang-bang just in case to compensate
    // for unachievable low velocities
    if (Robotable)
    {
        for (int i=0; i<v.length(); i++)
        {
            if ((v[i]>-minAbsVel) && (v[i]<minAbsVel) && (v[i]!=0.0))
            {
                // current error in the joint space
                double e=qd[i]-fbHead[i];

                if (e>0.0)
                    v[i]=minAbsVel;
                else if (e<0.0)
                    v[i]=-minAbsVel;
                else
                    v[i]=0.0;
            }
        }
    }

    // convert to degrees
    qddeg=CTRL_RAD2DEG*qd;
    qdeg =CTRL_RAD2DEG*fbHead;
    vdeg =CTRL_RAD2DEG*v;

    // send velocities to the robot
    if (Robotable && !(vdeg==vdegOld))
    {    
        velHead->velocityMove(vdeg.data());
        vdegOld=vdeg;
    }

    // print info
    printIter(xd,fp,qddeg,qdeg,vdeg,1.0);

    // send x,q through YARP ports
    Vector q(nJointsTorso+nJointsHead);
    int j;
    for (j=0; j<nJointsTorso; j++)
        q[j]=CTRL_RAD2DEG*fbTorso[j];
    for (; j<q.length(); j++)
        q[j]=qdeg[j-nJointsTorso];

    if (port_x.getOutputCount()>0)
        port_x.write(fp);

    if (port_q.getOutputCount()>0)
        port_q.write(q);

    // update pose information
    mutex.wait();

    for (int i=0; i<nJointsTorso; i++)
    {
        chainNeck->setAng(i,fbTorso[i]);
        chainEyeL->setAng(i,fbTorso[i]);
        chainEyeR->setAng(i,fbTorso[i]);
    }
    for (int i=0; i<3; i++)
    {
        chainNeck->setAng(nJointsTorso+i,fbHead[i]);
        chainEyeL->setAng(nJointsTorso+i,fbHead[i]);
        chainEyeR->setAng(nJointsTorso+i,fbHead[i]);
    }
    chainEyeL->setAng(nJointsTorso+3,fbHead[3]);               chainEyeR->setAng(nJointsTorso+3,fbHead[3]);
    chainEyeL->setAng(nJointsTorso+4,fbHead[4]+fbHead[5]/2.0); chainEyeR->setAng(nJointsTorso+4,fbHead[4]-fbHead[5]/2.0);

    mutex.post();

    // update joints angles
    fbHead=Int->integrate(v);
    commData->get_q()=fbHead;
    commData->get_torso()=fbTorso;
    commData->get_v()=v;
}


/************************************************************************/
void Controller::threadRelease()
{
    stopLimbsVel();

    port_x.interrupt();
    port_q.interrupt();

    port_x.close();
    port_q.close();

    delete neck;
    delete eyeL;
    delete eyeR;
    delete mjCtrlNeck;
    delete mjCtrlEyes;
    delete Int;

    if (alignLnkLeft1!=NULL)
        delete alignLnkLeft1;

    if (alignLnkLeft2!=NULL)
        delete alignLnkLeft2;

    if (alignLnkRight1!=NULL)
        delete alignLnkRight1;

    if (alignLnkRight2!=NULL)
        delete alignLnkRight2;
}


/************************************************************************/
void Controller::suspend()
{
    stopLimbsVel();

    fprintf(stdout,"\nController has been suspended!\n\n");

    RateThread::suspend();
}


/************************************************************************/
void Controller::resume()
{
    if (Robotable)
    {
        getFeedback(fbTorso,fbHead,encTorso,encHead);
    
        for (unsigned int i=0; i<3; i++)
        {
            fbNeck[i]=fbHead[i];
            fbEyes[i]=fbHead[3+i];
        }
    }

    fprintf(stdout,"\nController has been resumed!\n\n");

    RateThread::resume();
}


/************************************************************************/
double Controller::getTneck() const
{
    return neckTime;
}


/************************************************************************/
double Controller::getTeyes() const
{
    return eyesTime;
}


/************************************************************************/
void Controller::setTneck(const double execTime)
{
    double lowerThresNeck=eyesTime+0.2;
    if (execTime<lowerThresNeck)
    {        
        fprintf(stdout,"Warning: neck execution time is under the lower bound!\n");
        fprintf(stdout,"A new neck execution time of %g s is chosen\n",lowerThresNeck);

        neckTime=lowerThresNeck;
    }
    else
        neckTime=execTime;
}


/************************************************************************/
void Controller::setTeyes(const double execTime)
{
    double lowerThresEyes=10.0*Ts;
    if (execTime<lowerThresEyes)
    {        
        fprintf(stdout,"Warning: eyes execution time is under the lower bound!\n");
        fprintf(stdout,"A new eyes execution time of %g s is chosen\n",lowerThresEyes);

        eyesTime=lowerThresEyes;
    }
    else
        eyesTime=execTime;
}


/************************************************************************/
bool Controller::isMotionDone() const
{
    return !commData->get_isCtrlActive();
}


/************************************************************************/
void Controller::setTrackingMode(const bool f)
{
    commData->get_canCtrlBeDisabled()=!f;

    if (port_xd && f)
        port_xd->set_xd(fp);
}


/************************************************************************/
bool Controller::getTrackingMode() const 
{
    return !commData->get_canCtrlBeDisabled();
}


/************************************************************************/
bool Controller::getDesired(Vector &des) const
{
    des=qddeg;
    return true;
}


/************************************************************************/
bool Controller::getVelocity(Vector &vel) const
{
    vel=vdeg;
    return true;
}


/************************************************************************/
bool Controller::getPose(const string &poseSel, Vector &x)
{
    if (poseSel=="left")
    {
        mutex.wait();
        x=chainEyeL->EndEffPose();
        mutex.post();
        return true;
    }
    else if (poseSel=="right")
    {
        mutex.wait();
        x=chainEyeR->EndEffPose();
        mutex.post();
        return true;
    }
    else if (poseSel=="head")
    {
        mutex.wait();
        x=chainNeck->EndEffPose();
        mutex.post();
        return true;
    }
    else
        return false;
}


