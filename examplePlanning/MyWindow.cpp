#include "MyWindow.h"
#include "dynamics/SkeletonDynamics.h"
#include "dynamics/ContactDynamics.h"
#include "dynamics/BodyNodeDynamics.h"
#include "utils/UtilsMath.h"
#include "utils/Timer.h"
#include "yui/GLFuncs.h"
#include <cstdio>
#include "yui/GLFuncs.h"
#include "kinematics/BodyNode.h"
#include "kinematics/Shape.h"
#include "planning/PathPlanner.h"
#include "Controller.h"
#include "planning/Trajectory.h"
#include "robotics/Robot.h"
#include "kinematics/Dof.h"

using namespace Eigen;
using namespace kinematics;
using namespace utils;
using namespace integration;
using namespace dynamics;
using namespace std;
using namespace planning;

MyWindow::MyWindow(): Win3D() {
    DartLoader dl;
    mWorld = dl.parseWorld(DART_DATA_PATH"/scenes/hubo_box_world.urdf");
    
    // Add ground plane
    robotics::Object* ground = new robotics::Object();
    ground->addDefaultRootNode();
    dynamics::BodyNodeDynamics* node = new dynamics::BodyNodeDynamics();
    node->setShape(new kinematics::ShapeCube(Eigen::Vector3d(10.0, 10.0, 0.0001), 1.0));
    kinematics::Joint* joint = new kinematics::Joint(ground->getRoot(), node);
    ground->addNode(node);
    ground->initSkel();
    ground->update();
    ground->setImmobileState(true);
    mWorld->addObject(ground);
    mWorld->rebuildCollision();

    mBackground[0] = 1.0;
    mBackground[1] = 1.0;
    mBackground[2] = 1.0;
    mBackground[3] = 1.0;

    mPlayState = PAUSED;
    mSimFrame = 0;
    mPlayFrame = 0;

    mShowMarker = false;

    mTrans[2] = -2000.f;
    mEye = Eigen::Vector3d(2.0, -2.0, 2.0);
    mUp = Eigen::Vector3d(0.0, 0.0, 1.0);

    vector<int> trajectoryDofs(7);
    string trajectoryNodes[] = {"Body_RSP", "Body_RSR", "Body_RSY", "Body_REP", "Body_RWY", "rightUJoint", "rightPalmDummy"}; 
    for(int i = 0; i < 7; i++) {
        trajectoryDofs[i] = mWorld->getRobot(0)->getNode(trajectoryNodes[i].c_str())->getDof(0)->getSkelIndex();
    }

    vector<int> actuatedDofs(mWorld->getRobot(0)->getNumDofs() - 6);
    for(unsigned int i = 0; i < actuatedDofs.size(); i++) {
        actuatedDofs[i] = i + 6;
    }

    mWorld->getRobot(0)->getDof(19)->setValue(-10.0 * M_PI/180.0);
    mWorld->getRobot(0)->getDof(20)->setValue(-10.0 * M_PI/180.0);
    mWorld->getRobot(0)->getDof(23)->setValue(20.0 * M_PI/180.0);
    mWorld->getRobot(0)->getDof(24)->setValue(20.0 * M_PI/180.0);
    mWorld->getRobot(0)->getDof(27)->setValue(-10.0 * M_PI/180.0);
    mWorld->getRobot(0)->getDof(28)->setValue(-10.0 * M_PI/180.0);

    // Deactivate collision checking between the feet and the ground during planning
    mWorld->mCollisionHandle->getCollisionChecker()->deactivatePair(mWorld->getRobot(0)->getNode("leftFoot"), ground->getNode(1));
    mWorld->mCollisionHandle->getCollisionChecker()->deactivatePair(mWorld->getRobot(0)->getNode("rightFoot"), ground->getNode(1));

    VectorXd kI = 100.0 * VectorXd::Ones(mWorld->getRobot(0)->getNumDofs());
    VectorXd kP = 500.0 * VectorXd::Ones(mWorld->getRobot(0)->getNumDofs());
    VectorXd kD = 100.0 * VectorXd::Ones(mWorld->getRobot(0)->getNumDofs());
    vector<int> ankleDofs(2);
    ankleDofs[0] = 27;
    ankleDofs[1] = 28;
    const VectorXd anklePGains = -1000.0 * VectorXd::Ones(2);
    const VectorXd ankleDGains = -2000.0 * VectorXd::Ones(2);
    mController = new Controller(mWorld->getRobot(0), actuatedDofs, kP, kD, ankleDofs, anklePGains, ankleDGains);
    PathPlanner<> pathPlanner(*mWorld);
    VectorXd goal(7);
    goal << 0.0, -M_PI / 2.0, 0.0, -M_PI / 2.0, 0.0, 0.0, 0.0;
    list<VectorXd> path;
    if(!pathPlanner.planPath(0, trajectoryDofs, Eigen::VectorXd::Zero(7), goal, path)) {
        cout << "Path planner could not find a path" << endl;
    }
    else {
        const VectorXd maxVelocity = 0.3 * VectorXd::Ones(7);
        const VectorXd maxAcceleration = 0.3 * VectorXd::Ones(7);
        Trajectory* trajectory = new Trajectory(path, maxVelocity, maxAcceleration);
        cout << "Trajectory duration: " << trajectory->getDuration() << endl;
        mController->setTrajectory(trajectory, 0.1, trajectoryDofs);
    }

    mWorld->mCollisionHandle->getCollisionChecker()->activatePair(mWorld->getRobot(0)->getNode("leftFoot"), ground->getNode(1));
    mWorld->mCollisionHandle->getCollisionChecker()->activatePair(mWorld->getRobot(0)->getNode("rightFoot"), ground->getNode(1));

    std::cout << 
        "\nKeybindings:\n" <<
        "\n" <<
        "s: start or continue simulating.\n" <<
        "\n" <<
        "p: start or continue playback.\n" <<
        "r, t: move to start or end of playback.\n" <<
        "[, ]: step through playback by one frame.\n" <<
        "\n" <<
        "space: pause/unpause whatever is happening.\n" <<
        "\n" <<
        "q, escape: quit.\n" <<
        std::endl;
}

void MyWindow::retrieveBakedState(int frame)
{
    for (int i = 0; i < mWorld->getNumSkeletons(); i++) {
        int start = mWorld->mIndices[i];
        int size = mWorld->mDofs[i].size();
        mWorld->getSkeleton(i)->setPose(mBakedStates[frame].segment(start, size), false, false);
    }
}

void MyWindow::displayTimer(int _val)
{
    switch(mPlayState)
    {
    case PLAYBACK:
        if (mPlayFrame >= mBakedStates.size()) {
            mPlayFrame = 0;
        }
        retrieveBakedState(mPlayFrame);
        mPlayFrame++;
        glutPostRedisplay();
        glutTimerFunc(mDisplayTimeout, refreshTimer, _val);
        break;
    case SIMULATE:
        int numIter = (mDisplayTimeout / 1000.0) / mWorld->mTimeStep;
        for (int i = 0; i < numIter; i++) {
            mWorld->getSkeleton(0)->setInternalForces(mController->getTorques(mWorld->getRobot(0)->getPose(), mWorld->getRobot(0)->getQDotVector(), mWorld->mTime));
            mWorld->step();
        }
        mSimFrame += numIter;
        glutPostRedisplay();
        bake();
        glutTimerFunc(mDisplayTimeout, refreshTimer, _val);
        break;
    }
}

void MyWindow::draw()
{
    glDisable(GL_LIGHTING);

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    for(int i = 0; i < mWorld->getNumSkeletons(); i++) {
        mWorld->getSkeleton(i)->draw(mRI);
    }

    if(mPlayState == SIMULATE){
        glBegin(GL_LINES);    
        for (int k = 0; k < mWorld->mCollisionHandle->getCollisionChecker()->getNumContact(); k++) {
            Vector3d  v = mWorld->mCollisionHandle->getCollisionChecker()->getContact(k).point;
            Vector3d n = mWorld->mCollisionHandle->getCollisionChecker()->getContact(k).normal;
            glVertex3f(v[0], v[1], v[2]);
            glVertex3f(v[0] + n[0], v[1] + n[1], v[2] + n[2]);
        }
        glEnd();

        mRI->setPenColor(Vector3d(0.2, 0.2, 0.8));
        for (int k = 0; k < mWorld->mCollisionHandle->getCollisionChecker()->getNumContact(); k++) {
            Vector3d  v = mWorld->mCollisionHandle->getCollisionChecker()->getContact(k).point;
            mRI->pushMatrix();
            glTranslated(v[0], v[1], v[2]);
            mRI->drawEllipsoid(Vector3d(0.02, 0.02, 0.02));
            mRI->popMatrix();
        }
    }
    
    // display the frame count, the playback frame, and the movie length in 2D text
    glDisable(GL_LIGHTING);
    glColor3f(0.0,0.0,0.0);
    char buff[128];
    string frame;

    switch(mPlayState)
    {
    case PAUSED:
        sprintf(buff," ");
        break;
    case PLAYBACK:
        sprintf(buff,"Playing");
        break;
    case SIMULATE:
        sprintf(buff,"Simulating");
        break;
    }
    frame = string(buff);
    yui::drawStringOnScreen(0.02f,0.17f,frame);

    sprintf(buff,"Sim Frame: %d", mSimFrame);
    frame = string(buff);
    yui::drawStringOnScreen(0.02f,0.12f,frame);

    sprintf(buff,"Play Frame: %d", mPlayFrame);
    frame = string(buff);
    yui::drawStringOnScreen(0.02f,0.07f,frame);

    glEnable(GL_LIGHTING);
}

void MyWindow::keyboard(unsigned char key, int x, int y)
{
    switch(key){
    case ' ': // pause or unpause whatever's running; if space is the
              // first thing that's pressed, simulate
        if (mPlayState == PAUSED)
        {
            if (mPlayStateLast != PAUSED)
                mPlayState = mPlayStateLast;
            else
                mPlayState = SIMULATE;
            glutTimerFunc(mDisplayTimeout, refreshTimer, 0);
        }
        else
        {
            mPlayStateLast = mPlayState;
            mPlayState = PAUSED;
        }
        break;
    case 's': // switch to simulation mode
        if (mPlayState == SIMULATE) {
            mPlayState = PAUSED;
        }
        else {
            if (mPlayState == PAUSED)
                glutTimerFunc(mDisplayTimeout, refreshTimer, 0);
            mPlayState = SIMULATE;
            mPlayStateLast = SIMULATE;
        }
        break;
    case 'p': // switch to playback mode
        if (mPlayState == PLAYBACK) {
            mPlayState = PAUSED;
        }
        else {
            if (mPlayState == PAUSED)
                glutTimerFunc(mDisplayTimeout, refreshTimer, 0);
            mPlayState = PLAYBACK;
            mPlayStateLast = PLAYBACK;
        }
        break;

    case '[':
        if (mPlayState == PLAYBACK || mPlayStateLast == PLAYBACK)
        {
            mPlayFrame -= 1;
            if (mPlayFrame < 0) mPlayFrame = mBakedStates.size()-1;
            retrieveBakedState(mPlayFrame);
            glutPostRedisplay();
        }
        break;
    case ']':
        if (mPlayState == PLAYBACK || mPlayStateLast == PLAYBACK)
        {
            mPlayFrame += 1;
            if (mPlayFrame >= mBakedStates.size()) mPlayFrame = 0;
            retrieveBakedState(mPlayFrame);
            glutPostRedisplay();
        }
        break;
    case 'r': // set playback to the first frame
        mPlayFrame = 0;
        retrieveBakedState(mPlayFrame);
        glutPostRedisplay();
        break;
    case 't': // set playback motion to the newest simulated frame
        mPlayFrame = mBakedStates.size()-1;
        retrieveBakedState(mPlayFrame);
        glutPostRedisplay();
        break;
    case 'h': // show or hide markers
        mShowMarker = !mShowMarker;
        break;

    default:
        Win3D::keyboard(key,x,y);
    }
    glutPostRedisplay();
}

void MyWindow::bake()
{
    VectorXd state(mWorld->mIndices.back());
    for(int i = 0; i < mWorld->getNumSkeletons(); i++) {
        state.segment(mWorld->mIndices[i], mWorld->mDofs[i].size()) = mWorld->mDofs[i];
    }
    mBakedStates.push_back(state);
}
