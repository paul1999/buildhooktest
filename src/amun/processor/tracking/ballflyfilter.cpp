#include "ballflyfilter.h"
#include <cmath>
#include <iostream>
#include <Eigen/Core>
#include <Eigen/SVD>
#include <QDebug>

static const float floorDamping = 0.55; // robocup 2016: 0.67
static const int MAX_FRAMES_PER_FLIGHT = 200; // 60Hz, 3 seconds in the air
static const float ACCEPT_DIST = 0.35;
static const int APPROACH_SWITCH_FRAMENO = 16;

FlyFilter::FlyFilter(VisionFrame& frame, CameraInfo* cameraInfo) :
    m_shotDetected(false),
    m_initTime(frame.time),
    m_flyFitter(MAX_FRAMES_PER_FLIGHT),
    m_wasDetectedBefore(false)
{
    m_cameraInfo = cameraInfo;
    m_primaryCamera = -1; // is set with first frame processing
    resetFlightReconstruction();
}

FlyFilter::FlyFilter(const FlyFilter& f, qint32 primaryCamera):
    m_shotDetected(f.m_shotDetected),
    m_chipDetected(f.m_chipDetected),
    m_isActive(f.m_isActive),
    m_shotDetectionWindow(f.m_shotDetectionWindow),
    m_kickFrames(f.m_kickFrames),
    m_chipStartPos(f.m_chipStartPos),
    m_chipStartTime(f.m_chipStartTime),
    m_groundSpeed(f.m_groundSpeed),
    m_zSpeed(f.m_zSpeed),
    m_z0(f.m_z0),
    m_touchdownPos(f.m_touchdownPos),
    m_bouncing(f.m_bouncing),
    m_bounceStartTime(f.m_bounceStartTime),
    m_bounceZSpeed(f.m_bounceZSpeed),
    m_bounceStartPos(f.m_bounceStartPos),
    m_bounceGroundSpeed(f.m_bounceGroundSpeed),
    m_shotCamPos(f.m_shotCamPos),
    m_shotStartFrame(f.m_shotStartFrame),
    m_distToStartPos(f.m_distToStartPos),
    m_initTime(f.m_initTime),
    m_flyFitter(f.m_flyFitter),
    m_pinvDataInserted(f.m_pinvDataInserted),
    m_d_detailed(f.m_d_detailed),
    m_D_detailed(f.m_D_detailed),
    m_d_coarseControl(f.m_d_coarseControl),
    m_D_coarseControl(f.m_D_coarseControl),
    m_wasDetectedBefore(f.m_wasDetectedBefore),
    m_lastPredictionTime(f.m_lastPredictionTime)
{
    m_primaryCamera = primaryCamera;
    m_cameraInfo = f.m_cameraInfo;
}

Eigen::Vector3f FlyFilter::unproject(const ChipDetection& detection, float ballRadius) {
    float f = m_cameraInfo->focalLength.value(detection.cameraId);
    float a = detection.ballArea;
    float distInferred = f * (ballRadius/sqrt(a/M_PI) + 1) / 1000.0;
    Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(detection.cameraId);
    Eigen::Vector3f ballGround = Eigen::Vector3f(detection.ballPos(0), detection.ballPos(1), 0);
    // set calculated length on direction from camera to reported
    return cam + (ballGround - cam).normalized() * distInferred;
}

static bool monotonicRisingOneException(const QList<float>& points)
{
    int exceptions = 0;
    for (int i=0; i<points.size()-1; i++) {
        if (points.at(i+1) > points.at(i)){
            exceptions++;
        }
    }
    return exceptions < 2;
}

bool FlyFilter::checkIsShot()
{
    if (m_shotDetectionWindow.size() < 4) {
        return false;
    }

    float dribblerDist0 = (m_shotDetectionWindow.at(0).dribblerPos - m_shotDetectionWindow.at(0).ballPos).norm();
    float dribblerDist1 = (m_shotDetectionWindow.at(0).dribblerPos - m_shotDetectionWindow.at(1).ballPos).norm();
    float dribblerDist2 = (m_shotDetectionWindow.at(0).dribblerPos - m_shotDetectionWindow.at(2).ballPos).norm();
    float dribblerDist3 = (m_shotDetectionWindow.at(0).dribblerPos - m_shotDetectionWindow.at(3).ballPos).norm();

    double dist = (m_shotDetectionWindow.at(1).ballPos - m_shotDetectionWindow.at(3).ballPos).norm();
    double timeDiff = (m_shotDetectionWindow.at(3).time-m_shotDetectionWindow.at(1).time) / 1000000000.0;
    float absSpeed = dist/timeDiff;

    debug("shot/d speed 0", m_shotDetectionWindow.at(0).dribblerSpeed);
    debug("shot/d speed 1", m_shotDetectionWindow.at(1).dribblerSpeed);
    debug("shot/a speed 0", m_shotDetectionWindow.at(0).absSpeed);
    debug("shot/a speed 1", m_shotDetectionWindow.at(1).absSpeed);

    debug("shot/abs speed", absSpeed);
    debug("shot/d dist0", dribblerDist0);
    debug("shot/d dist1", dribblerDist1);
    debug("shot/d dist2", dribblerDist2);
    debug("shot/d dist3", dribblerDist3);
    if (
            ((m_shotDetectionWindow.at(1).dribblerSpeed > m_shotDetectionWindow.at(0).dribblerSpeed)
            && m_shotDetectionWindow.at(1).dribblerSpeed > 0.1
            && absSpeed > 1
            && m_shotDetectionWindow.at(1).absSpeed - m_shotDetectionWindow.at(0).absSpeed > 0.2
            )) {
        if (
            // distance monotonic rising
            dribblerDist0 < dribblerDist1
            && dribblerDist1 < dribblerDist2
            && dribblerDist2 < dribblerDist3
            // moved at least 6cm
            && dribblerDist3 - dribblerDist0 > 0.06
            // initial ball pos close to dribbler
            && dribblerDist0 < 0.1
        ){
            if (m_shotDetectionWindow.at(0).dribblerSpeed > 0.1) {
                m_shotStartFrame = 0;
            } else {
                m_shotStartFrame = 1;
            }
            return true;
        }
    }
    return false;
}

unsigned FlyFilter::numMeasurementsWithOwnCamera()
{
    int num = 0;
    for (int i=0; i<m_kickFrames.size(); i++) {
        if (m_kickFrames.at(i).cameraId == m_kickFrames.back().cameraId) {
            num++;
        }
    }
    return num;
}

bool FlyFilter::collision() {
    if (m_kickFrames.size() < 3) {
        return false;
    }
    auto& first = m_kickFrames.at(m_kickFrames.size()-3);
    auto& second = m_kickFrames.at(m_kickFrames.size()-2);
    auto& third = m_kickFrames.at(m_kickFrames.size()-1);

    float angle = fabs(atan2(first.ballPos(1)-second.ballPos(1), first.ballPos(0)-second.ballPos(0))
        - atan2(third.ballPos(1)-second.ballPos(1), third.ballPos(0)-second.ballPos(0)));
    debug("collision angle", angle);

    float robotDist = (m_kickFrames.back().ballPos - m_kickFrames.back().robotPos).norm();
    debug("collision dist", robotDist);
    float height = 0;
    if(m_isActive) {
        height = predictTrajectory(m_lastPredictionTime).pos(2);
    }

    float collision = (angle < 0.86*M_PI || angle > 1.14*M_PI) && height < 0.15 && robotDist < 0.18;
    if (collision) {
        debugCircle("coll 1", first.ballPos(0), first.ballPos(1), 0.02);
        debugCircle("coll 2", second.ballPos(0), second.ballPos(1), 0.02);
        debugCircle("coll 3", third.ballPos(0), third.ballPos(1), 0.02);
    }
    return collision;
}

FlyFilter::PinvResult FlyFilter::calcPinvAndIntersection()
{
    ChipDetection firstInTheAir = m_kickFrames.at(m_shotStartFrame);

    double lowerTimeBound = firstInTheAir.time;
    if (m_pinvDataInserted == 0) {
        m_pinvDataInserted = m_shotStartFrame;
    }
    for (int i=m_pinvDataInserted; i<m_kickFrames.size(); i++) {
        Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(m_kickFrames.at(i).cameraId);
        float x0 = cam(0) - firstInTheAir.ballPos(0);
        float y0 = cam(1) - firstInTheAir.ballPos(1);
        double time = m_kickFrames.at(i).time - lowerTimeBound;
        double x = m_kickFrames.at(i).ballPos(0);
        double y = m_kickFrames.at(i).ballPos(1);
        double t_i = time / 1000000000.0; // seconds
        double alpha = (x-cam(0)) / cam(2);
        double beta = (y-cam(1)) / cam(2);

        m_D_detailed(i*2, 0) = alpha;
        m_D_detailed(i*2, 1) = alpha*t_i;
        m_D_detailed(i*2, 2) = 1;
        m_D_detailed(i*2, 3) = t_i;
        m_D_detailed(i*2, 4) = 0;
        m_D_detailed(i*2, 5) = 0;
        m_d_detailed(i*2) = 0.5*9.81*alpha*t_i*t_i + x;

        m_D_detailed(i*2+1, 0) = beta;
        m_D_detailed(i*2+1, 1) = beta*t_i;
        m_D_detailed(i*2+1, 2) = 0;
        m_D_detailed(i*2+1, 3) = 0;
        m_D_detailed(i*2+1, 4) = 1;
        m_D_detailed(i*2+1, 5) = t_i;
        m_d_detailed(i*2+1) = 0.5*9.81*beta*t_i*t_i + y;

        m_D_coarseControl(i*2, 0) = alpha; //z0
        m_D_coarseControl(i*2, 1) = alpha*t_i; // vz
        m_D_coarseControl(i*2, 2) = -t_i; // vx
        m_D_coarseControl(i*2, 3) = 0; // vy
        m_d_coarseControl(i*2) = 0.5*9.81*alpha*t_i*t_i - x0;

        m_D_coarseControl(i*2+1, 0) = beta; // z0
        m_D_coarseControl(i*2+1, 1) = beta*t_i; // vz
        m_D_coarseControl(i*2+1, 2) = 0; //vx
        m_D_coarseControl(i*2+1, 3) = -t_i; // vy
        m_d_coarseControl(i*2+1) = 0.5*9.81*beta*t_i*t_i - y0;
        m_pinvDataInserted = i;
    }

    Eigen::VectorXf pi = m_D_detailed.colPivHouseholderQr().solve(m_d_detailed);
    Eigen::VectorXf piControl = m_D_coarseControl.colPivHouseholderQr().solve(m_d_coarseControl);

    PinvResult res;
    res.x0 = pi(2);
    res.y0 = pi(4);
    res.z0 = pi(0);
    res.vx = pi(3);
    res.vy = pi(5);
    res.vz = pi(1);

    debug("pinv_params/x0", res.x0);
    debug("pinv_params/y0", res.y0);
    debug("pinv_params/vx", res.vx);
    debug("pinv_params/vy", res.vy);
    debug("pinv_params/vz", res.vz);
    debug("pinv_params/z0", res.z0);

    debug("pinv_params/z0 control", piControl(0));
    debug("pinv_params/vx control", piControl(2));
    debug("pinv_params/vy control", piControl(3));
    debug("pinv_params/vz control", piControl(1));

    res.vxControl = piControl(2);
    res.vyControl = piControl(3);

    Eigen::Vector2f startPos = firstInTheAir.ballPos;
    Eigen::Vector2f calculatedStartPos = Eigen::Vector2f(res.x0, res.y0);
    float distStartPos = (calculatedStartPos-startPos).norm();
    if (!m_bouncing) {
        m_distToStartPos = distStartPos; // is used for filter choice
    }
    debugCircle("calc start pos", calculatedStartPos(0), calculatedStartPos(1), 0.04);
    res.distStartPos = distStartPos;
    debug("pinv_params/dist start pos", res.distStartPos);

    Eigen::Vector2f vGroundPinv(res.vx, res.vy);
    Eigen::Vector2f endPos = firstInTheAir.ballPos + vGroundPinv;
    debugLine("dirDet", m_kickFrames.first().ballPos(0), m_kickFrames.first().ballPos(1), endPos(0), endPos(1));

    Eigen::Vector2f vGroundPinvControl(res.vxControl, res.vyControl);
    Eigen::Vector2f endPosC = firstInTheAir.ballPos + vGroundPinvControl;
    debugLine("dirCoa", m_kickFrames.first().ballPos(0), m_kickFrames.first().ballPos(1), endPosC(0), endPosC(1), 1);

    debug("pinv_params/ground speed pinv control", vGroundPinvControl.norm());
    debug("pinv_params/ground speed pinv", vGroundPinv.norm());

    // intersection approach
    Eigen::Vector2f vGround;
    if (m_kickFrames.size() < 10 && m_kickFrames.at(m_shotStartFrame).absSpeed < 1) {
        vGround = m_kickFrames.at(m_shotStartFrame).ballPos-m_kickFrames.at(m_shotStartFrame).robotPos;
        debug("intersection dir", "ball to robot");
    } else {
        debug("intersection dir", "pinv");
        vGround = Eigen::Vector2f(res.vx, res.vy);
        if (res.vx*res.vxControl < 0 && res.vy*res.vyControl < 0) {
            vGround = Eigen::Vector2f(res.vxControl, res.vyControl);
            debug("correction", true);
        }
    }

    Eigen::Vector2f S = m_kickFrames.at(m_shotStartFrame).ballPos;
    Eigen::Vector2f V = S+vGround;
    qint64 startTime = m_kickFrames.at(m_shotStartFrame).time;

    int numZSpeeds = 0;
    double zSpeed = 0;

    double groundSpeedLength = 0;
    for (int i=m_shotStartFrame+1; i < m_kickFrames.size(); i++) {
        auto& f = m_kickFrames.at(i);
        Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(f.cameraId);

        Eigen::Vector2f K(cam(0), cam(1));
        Eigen::Vector2f P = f.ballPos;

        debugLine("sp", S(0), S(1), V(0), V(1), 1);
        debugLine("pr", K(0), K(1), P(0), P(1), 2);
        double numerator = (K(1)-S(1))/(V(1)-S(1)) - (K(0)-S(0))/(V(0)-S(0));
        double denominator = (P(0)-K(0))/(V(0)-S(0)) - (P(1)-K(1))/(V(1)-S(1));
        double mu = numerator/denominator;
        Eigen::Vector2f intersection = K + (P-K) * mu;
        debugCircle("intersection", intersection(0), intersection(1), 0.04);

        auto timeDiff = ((f.time - startTime) / 1000000000.0);
        double speed = (S - intersection).norm() / timeDiff;
        groundSpeedLength += speed;

        float H = cam(2);
        float d = (P - intersection).norm();
        float D = (K - P).norm();
        float h = (H*d) / D; // intersect theorem

        zSpeed += h/timeDiff + 4.9*timeDiff;
        numZSpeeds++;
    }
    groundSpeedLength /= (m_kickFrames.size()-m_shotStartFrame-1);
    zSpeed /= numZSpeeds;

    res.intersectionGroundSpeed = vGround.normalized() * groundSpeedLength;
    res.intersectionZSpeed = zSpeed;
    debug("approx/z speed", res.intersectionZSpeed);

    Eigen::Vector2f VC = S + vGroundPinvControl;
    Eigen::Vector2f VP = S + vGroundPinv;
    float vControlDiff = fabs(atan2(VP(1)-S(1), VP(0)-S(0)) - atan2(VC(1)-S(1), VC(0)-S(0)));
    debug("approx/vControlDiff", vControlDiff);
    res.vControlDiff = vControlDiff;


    if (firstInTheAir.cameraId != m_kickFrames.back().cameraId) {
        // for a correct refSpeed, search first measurement from current camera
        for (auto& m: m_kickFrames) {
            if (m.cameraId == m_kickFrames.back().cameraId && m.ballPos(0) != m_kickFrames.back().ballPos(0)) {
                firstInTheAir = m;
                break;
            }
        }
    }
    float refSpeed = (firstInTheAir.ballPos - m_kickFrames.back().ballPos).norm()
                / ((m_kickFrames.back().time - firstInTheAir.time)/1000000000.0);
    debug("pinv_params/ground speed raw", refSpeed);
    res.refSpeed = refSpeed;

    return res;
}

void FlyFilter::approachPinvApply(const PinvResult &pinvRes)
{
    ChipDetection firstInTheAir = m_kickFrames.at(m_shotStartFrame);
    m_chipStartPos = firstInTheAir.ballPos;
    m_chipStartTime = firstInTheAir.time;
    const float g = 9.81;
    const float z0 = pinvRes.z0;
    const float vz = pinvRes.vz;

    m_groundSpeed = Eigen::Vector2f(pinvRes.vx, pinvRes.vy);
    m_zSpeed = pinvRes.vz;

    if (pinvRes.distStartPos < 0.06) {
        m_chipStartPos = Eigen::Vector2f(pinvRes.x0, pinvRes.y0);
    }

    const float t1 = (vz + sqrt(vz*vz + g*z0*2)) / g;
    const float t2 = (vz - sqrt(vz*vz + g*z0*2)) / g;
    const float T = (t1 < t2) ? t1 : t2;
    debug("pinv/t2", t2);
    debug("pinv/t1", t1);
    debug("pinv/T", T);

    if (std::isnan(T)) {
        debug("abort pinv", T);
        resetFlightReconstruction();
        return;
    }

    if (pinvRes.distStartPos < 0.06) {
        if (fabs(T) < 0.08) {
            m_chipStartPos = m_chipStartPos+m_groundSpeed*T;
        }
    }

    if (fabs(T) < 0.08) {
        m_zSpeed = pinvRes.vz - 9.81*T;
    }

    if (fabs(T) < 0.04) { // maximum error 20ms at 50Hz
        m_chipStartTime = firstInTheAir.time + T*1000000000;
    }
}

void FlyFilter::approachIntersectApply(const FlyFilter::PinvResult &pinvRes)
{
    ChipDetection firstInTheAir = m_kickFrames.at(m_shotStartFrame);
    m_chipStartPos = m_kickFrames.at(m_shotStartFrame).ballPos;
    m_chipStartTime = firstInTheAir.time-10000000; // -10ms, actual kick was before
    m_groundSpeed = pinvRes.intersectionGroundSpeed;
    m_zSpeed = pinvRes.intersectionZSpeed;
    debug("method intersect", true);
    debug("approx/speed length", pinvRes.intersectionGroundSpeed.norm());
}

bool FlyFilter::approachAreaApply()
{
    ChipDetection firstInTheAir = m_kickFrames.at(m_shotStartFrame);
    m_chipStartPos = m_kickFrames.at(m_shotStartFrame).ballPos;
    m_chipStartTime = firstInTheAir.time;
    if (m_kickFrames.size() < m_shotStartFrame+4) {
        return false;
    }

    float ballRadius = 0;
    int startR = m_shotStartFrame+1;
    int endR = m_shotStartFrame+4;
    for (int i=startR; i<endR; i++) {
        Eigen::Vector3f ballPos(m_kickFrames.at(i).ballPos(0), m_kickFrames.at(i).ballPos(1), 0);
        Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(m_kickFrames.at(i).cameraId);
        float d = (ballPos - cam).norm() * 1000 -100; // mm
        float focalLength = m_cameraInfo->focalLength.value(m_kickFrames.at(i).cameraId);
        float r = (d / focalLength - 1) * sqrt(m_kickFrames.at(i).ballArea/M_PI);
        debug(QString("ball radius")+QString::number(i), r);
        ballRadius += r;
    }
    ballRadius /= (endR-startR);
    debug("ball radius", ballRadius);
    if (ballRadius == 0) { // happens in the simulator
        return false;
    }

    double speedXSum = 0.0;
    double speedYSum = 0.0;
    int start = m_shotStartFrame+2; // m_shotStartFrame+1 is first in the air
    int end = m_kickFrames.size();
    int num = end-start;
    for (int i=start; i<end; i++) {
        auto& m = m_kickFrames.at(i);
        auto timeDiff = (m.time - firstInTheAir.time) / 1000000000.0;
        Eigen::Vector3f unprojPos = unproject(m, ballRadius);
        double xDist = unprojPos(0) - firstInTheAir.ballPos(0);
        double yDist = unprojPos(1) - firstInTheAir.ballPos(1);
        speedXSum += xDist/timeDiff;
        speedYSum += yDist/timeDiff;
    }

    Eigen::Vector2f speed(speedXSum/num, speedYSum/num);
    debug("height/vx", speed(0));
    debug("height/vy", speed(1));
    debug("height/v total", speed.norm());

    m_groundSpeed = speed;//dir.normalized()*speedLengthViaHeight;

    debugLine("calc dir", firstInTheAir.ballPos(0), firstInTheAir.ballPos(1), firstInTheAir.ballPos(0)+speed(0), firstInTheAir.ballPos(1)+speed(1));

    qint64 startTime = m_kickFrames.at(m_shotStartFrame).time;
    for (int i=start; i<end; i++) {
        auto& m = m_kickFrames.at(i);
        double time = (m.time - startTime)/1000000000.0;
        Eigen::Vector3f unprojPos = unproject(m, ballRadius);
        double height = unprojPos(2);


        m_flyFitter.addPoint(time, height);
    }
    QuadraticLeastSquaresFitter::QuadraticFitResult res = m_flyFitter.fit();
    debug("height/res b", res.b);
    m_zSpeed = res.b;

    debug("method height", true);

    return false;
}

// return value in the interval [0,pi] radians
static double innerAngle(Eigen::Vector2f center, Eigen::Vector2f A, Eigen::Vector2f B)
{
    float dx21 = A(0)-center(0);
    float dx31 = B(0)-center(0);
    float dy21 = A(1)-center(1);
    float dy31 = B(1)-center(1);
    float m12 = sqrt( dx21*dx21 + dy21*dy21 );
    float m13 = sqrt( dx31*dx31 + dy31*dy31 );
    return acos( (dx21*dx31 + dy21*dy31) / (m12 * m13) );
}

bool FlyFilter::approachPinvApplicable(const FlyFilter::PinvResult &pinvRes)
{
    Eigen::Vector2f vGroundPinv(pinvRes.vx, pinvRes.vy);
    Eigen::Vector2f center = m_kickFrames.first().ballPos;
    auto vToProj = innerAngle(center, m_kickFrames.back().ballPos, center+vGroundPinv);
    debug("vToProjPinv", vToProj);

    float z0 = pinvRes.z0;
    float vz = pinvRes.vz;
    if (z0 > -0.5 && (z0 < 1 || (m_isActive && z0 < 4)) && vz > 1
        && pinvRes.distStartPos < 0.4 && vz < 10
        && (std::isnan(vToProj) || vToProj < 0.7)
    ){
        return true;
    }
    return false;
}

bool FlyFilter::approachIntersectApplicable(const FlyFilter::PinvResult &pinvRes)
{
    // the calulated speed direction should not differ to much from the projection
    Eigen::Vector2f center = m_kickFrames.first().ballPos;
    auto vToProj = innerAngle(center, m_kickFrames.back().ballPos, center+pinvRes.intersectionGroundSpeed);
    debug("vToProjIntersection", vToProj);

    // calculated direction has to lie between projection and camera
    Eigen::Vector3f cam3d = m_cameraInfo->cameraPosition.value(m_kickFrames.back().cameraId);
    Eigen::Vector2f cam(cam3d(0), cam3d(1));
    auto angleSpeed = innerAngle(center, cam, center+pinvRes.intersectionGroundSpeed);
    auto angleProjection = innerAngle(center, cam, m_kickFrames.back().ballPos);
    debug("angle v", angleSpeed);
    debug("angle proj", angleProjection);

    return angleSpeed < angleProjection && vToProj < 0.7;
}

void FlyFilter::parabolicFlightReconstruct(const PinvResult& pinvRes)
{
    if (approachPinvApplicable(pinvRes) && m_kickFrames.size() > APPROACH_SWITCH_FRAMENO) {
        debug("chip approach", "pinv");
        approachPinvApply(pinvRes);
        m_isActive = true;
    } else {
        Eigen::Vector2f lastBall = m_kickFrames.back().ballPos;
        Eigen::Vector3f cam3d = m_cameraInfo->cameraPosition.value(m_kickFrames.back().cameraId);
        Eigen::Vector2f cam(cam3d(0), cam3d(1));
        Eigen::Vector2f center = m_kickFrames.first().ballPos;
        auto intersectionAngle = innerAngle(center, cam, lastBall);
        debug("intersection angle", intersectionAngle);

        if (intersectionAngle < 0.4) { // angle low
            debug("chip approach", "height");
            approachAreaApply();
            m_isActive = true;
        } else if (approachIntersectApplicable(pinvRes)) {
            debug("chip approach", "intersection");
            approachIntersectApply(pinvRes);
            m_isActive = true;
        } else {
            debug("chip approach", "unavailable");
            m_isActive = false;
        }
    }
}

bool FlyFilter::detectionCurviness(const PinvResult& pinvRes)
{
    if (m_kickFrames.size() < 5) {
        return false;
    }

    Eigen::Vector3f camPos = m_cameraInfo->cameraPosition.value(m_kickFrames.first().cameraId);
    Eigen::Vector2f arb = m_kickFrames.first().robotPos;
    Eigen::Vector2f b = arb + (m_kickFrames.first().ballPos - arb).normalized();
    debugLine("chip angle detection", arb(0), arb(1), b(0), b(1));

    double sumXY = 0;
    double sumX = 0;
    double sumY = 0;
    double sumXSq = 0;
    double sumYSq = 0;
    int n = m_kickFrames.size();
    for (auto& m : m_kickFrames) {
        sumX += m.ballPos(0);
        sumY += m.ballPos(1);
        sumXY += m.ballPos(0) * m.ballPos(1);
        sumXSq += m.ballPos(0)*m.ballPos(0);
        sumYSq += m.ballPos(1)*m.ballPos(1);
    }
    // pearson correlation coefficient
    double r = (n*sumXY-sumX*sumY) / (sqrt(n*sumXSq-sumX*sumX) * sqrt(n*sumYSq-sumY*sumY));
    debug("detection/correlation", r);

    QList<double> angles;
    Eigen::Vector2f dp = m_kickFrames.at(0).ballPos;
    for (int j=2; j<m_kickFrames.size(); j++) {
        // start at 2 because first angle is too noisy
        Eigen::Vector2f ball = m_kickFrames.at(j).ballPos;
        double angle = atan2(camPos(1) - dp(1), camPos(0) - dp(0)) -
                        atan2(ball(1) - dp(1), ball(0) - dp(0));
        angle = fmod(angle + 2*M_PI, 2*M_PI); // norm to 0..2pi
        angles.append(angle);
    }

    double xSum = 0;
    double angleSum = 0;
    double xSumSq = 0;
    double angleXSum = 0;
    for(int i=0; i<angles.size(); i++) {
        xSum += i;
        xSumSq += i*i;
        angleSum += angles.at(i);
        angleXSum += i*angles.at(i);
    }
    double slope = (angles.size() * angleXSum - xSum * angleSum) / (angles.size() * xSumSq - xSum * xSum);

    debug("detection angle/slope", slope);

    if (m_kickFrames.size() < 8 && pinvRes.refSpeed < 2) {
        // reflection shots often have a distinct slope at low speeds
        return false;
    }

    return fabs(slope) > std::max(-0.03212*m_kickFrames.size() + 0.4873, 0.06);
}

bool FlyFilter::detectionHeight()
{
    if (m_kickFrames.size() < 5) {
        return false;
    }

    if (m_kickFrames.back().cameraId != m_kickFrames.front().cameraId) {
        // if camera changed, and we have not detected a chip yet,
        // assumptions about the ball radius become invalid
        return false;
    }

    float ballRadius = 0;
    int startR = m_shotStartFrame+1;
    int endR = m_shotStartFrame+4;

    for (int i=startR; i<endR; i++) {
        Eigen::Vector3f ballPos(m_kickFrames.at(i).ballPos(0), m_kickFrames.at(i).ballPos(1), 0);
        Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(m_kickFrames.at(i).cameraId);
        float d = (ballPos - cam).norm() * 1000 - 50; // mm
        float focalLength = m_cameraInfo->focalLength.value(m_kickFrames.at(i).cameraId);
        float r = (d / focalLength - 1) * sqrt(m_kickFrames.at(i).ballArea/M_PI);
        debug(QString("ball radius")+QString::number(i), r);
        ballRadius += r;
    }
    ballRadius /= (endR-startR);

    QList<float> heights;
    for(auto& m : m_kickFrames) {
        heights.append(unproject(m, ballRadius)(2));
    }
    float low = heights.at(0)+heights.at(1);
    float high = heights.at(heights.size()-2)+heights.at(heights.size()-1);

    debug("detection height/high", high);
    debug("detection height/diff", high-low);
    if (m_kickFrames.size() > 6 && monotonicRisingOneException(heights)){
        debug("detection height/mon", true);
        return high > 0.5 && high-low > 0.5;
    }
    return high > 1 && high-low > 1;
}

bool FlyFilter::detectionSpeed()
{
    QVector<float> speeds;
    for (int i=1; i<m_kickFrames.size(); i++) {
        if (m_kickFrames.at(i).cameraId != m_kickFrames.back().cameraId) {
            // bad geometry calibration may lead to virtual accelerations
            continue;
        }
        double dist = (m_kickFrames.at(i).ballPos - m_kickFrames.at(i-1).ballPos).norm();
        double timeDiff = (m_kickFrames.at(i).time-m_kickFrames.at(i-1).time) / 1000000000.0; //seconds
        speeds.append(dist/timeDiff);
    }
    float avg = std::accumulate(speeds.begin(), speeds.end(), 0.0)/speeds.size();

    double xSum = 0;
    double valSum = 0;
    double xSumSq = 0;
    double valXSum = 0;
    int n = speeds.size()-1;
    for(int i=1; i<speeds.size(); i++) {
        if (speeds.at(i) > 1.4*avg) {
            debug("detection speed/leave out", true);
            n--;
            continue;
        }
        xSum += i;
        xSumSq += i*i;
        valSum += speeds.at(i);
        valXSum += i*speeds.at(i);
    }
    double slope = (n * valXSum - xSum * valSum) / (n * xSumSq - xSum * xSum);
    slope /= valSum / n;

    debug("detection speed/slope", slope);
    debug("detection speed/avg", avg);
    debug("detection speed/last", speeds.back());

    return (slope > 0.02 && speeds.size() > 5)
            || (slope > 0.002 && speeds.size() > 14);
}

bool FlyFilter::detectionPinv(const FlyFilter::PinvResult &pinvRes)
{
    const auto g = 9.81;
    float z0 = pinvRes.z0;
    float vz = pinvRes.vz;
    Eigen::Vector2f vGroundPinv(pinvRes.vx, pinvRes.vy);

    float vPinvSq = pinvRes.vx*pinvRes.vx + pinvRes.vy*pinvRes.vy;
    float vContrSq = pinvRes.vxControl*pinvRes.vxControl + pinvRes.vyControl*pinvRes.vyControl;
    float vRawSq = pinvRes.refSpeed*pinvRes.refSpeed;
    float vMean = (vPinvSq + vContrSq + vRawSq) / 3;
    debug("pinv detection/vPinvSq", vPinvSq);
    debug("pinv detection/vContrSq", vContrSq);
    debug("pinv detection/vRawSq", vRawSq);
    debug("pinv detection/vMean", vMean);

    float maxFlightDurationHalf = vz/g;
    float maxFlightDuration = maxFlightDurationHalf*2;
    float maxHeight = vz*maxFlightDurationHalf - (g/2) *maxFlightDurationHalf*maxFlightDurationHalf;
    double timeElapsed = (m_kickFrames.back().time - m_chipStartTime) / 1000000000.0;

    float flightDistGroundCalc = vz*timeElapsed;
    float flightDistMeasured = (m_kickFrames.front().ballPos - m_kickFrames.back().ballPos).norm();
    debug("pinv detection/flight dist calc", flightDistGroundCalc);
    debug("pinv detection/flight dist measured", flightDistMeasured);
    debug("pinv detection/t", timeElapsed);
    debug("pinv detection/max flightDuration", maxFlightDuration);
    debug("pinv detection/max height", maxHeight);

    if (m_kickFrames.front().cameraId != m_kickFrames.back().cameraId) {
        debug("pinv detection/cameraChange", true);
        if (maxHeight < 0.5) {
            // camera changes lead to false detections, probably because of
            // geometry calibration differences
            return false;
        }
    }

    if (z0 > -0.4 && z0 < 1.5 && vz > 1 && vz < 10
        && pinvRes.distStartPos < 1
        && vGroundPinv.norm() > 1.5
        && timeElapsed < maxFlightDuration
        && maxHeight > 0.1
        && fabs(flightDistGroundCalc-flightDistMeasured) < std::min(flightDistGroundCalc, flightDistMeasured)/3
        && m_kickFrames.size() > 5
    ){
        return true;
    }
    return false;
}

void FlyFilter::processVisionFrame(const VisionFrame& frame)
{
    if (frame.cameraId != m_primaryCamera) {
        m_primaryCamera = frame.cameraId;
    }

    Eigen::Vector2f reportedBallPos(frame.x, frame.y);
    float timeSinceInit = (frame.time-m_initTime);
    float dribblerSpeed = 0;
    float absSpeed = 0;

    float dribblerDist = (frame.dribblerPos - reportedBallPos).norm();

    if (m_shotDetectionWindow.size() > 0) {
        float timeDiff = timeSinceInit/1000000000 - m_shotDetectionWindow.back().time/1000000000; // seconds
        float lastDribblerDist = (m_shotDetectionWindow.back().dribblerPos-m_shotDetectionWindow.back().ballPos).norm();
        dribblerSpeed = (dribblerDist-lastDribblerDist) / timeDiff;
        absSpeed = (reportedBallPos-m_shotDetectionWindow.back().ballPos).norm() / timeDiff;
    }

    ChipDetection currentDetection(dribblerSpeed, absSpeed, timeSinceInit, reportedBallPos, frame.dribblerPos, frame.ballArea, frame.robotPos, frame.cameraId);
    m_shotDetectionWindow.append(currentDetection);
    if (m_shotDetectionWindow.size() > 4) {
        m_shotDetectionWindow.pop_front();
    }

    debug("chip measurements", m_kickFrames.size());
    if (m_kickFrames.empty() && checkIsShot()) {
        m_shotCamPos = m_cameraInfo->cameraPosition.value(frame.cameraId);

        m_kickFrames.append(m_shotDetectionWindow.at(0));
        m_kickFrames.append(m_shotDetectionWindow.at(1));
        m_kickFrames.append(m_shotDetectionWindow.at(2));
        // currentDetection is also in m_shotDetectionWindow but will be added by chip detection
        m_shotDetectionWindow.clear();
        // we need to keep the last measurement to infer speed
        m_shotDetectionWindow.append(m_kickFrames.back());

        debug("shot detected", 1);
        m_shotDetected = true;
    }

    if (m_kickFrames.size() > 0) { // chip detection or tracking ongoing
        m_kickFrames.append(currentDetection);

        if (collision()) {
            debug("abort collision", true);
            resetFlightReconstruction();
            return;
        }
        if(!m_bouncing){
            debug("chip detected", m_chipDetected);
            PinvResult pinvRes = calcPinvAndIntersection();
            if (!m_chipDetected) {
                bool heightSaysChip = detectionHeight(); // run for debug info
                bool isCurvy = detectionCurviness(pinvRes);                
                if (detectionSpeed()) {
                    Eigen::Vector3f cam3d = m_cameraInfo->cameraPosition.value(m_kickFrames.back().cameraId);
                    Eigen::Vector2f cam(cam3d(0), cam3d(1));
                    auto angleToCam = innerAngle(m_kickFrames.first().ballPos,
                        cam, m_kickFrames.back().ballPos);
                    debug("detection/speed", true);

                    if (numMeasurementsWithOwnCamera() > 10) { // MAGIC
                        debug("detection/speed", true);
                        m_chipDetected = true;
                    }
                    debug("angle to cam", angleToCam);
                    if (angleToCam > 0.45 ) { // MAGIC
                        if (isCurvy){
                            debug("detection/angle", true);                                                        
                            m_chipDetected = true;
                        }
                    } else {
                        if (heightSaysChip) {
                            debug("detection/height", true);                                                      
                            m_chipDetected = true;
                        }
                    }
                }
                if (detectionPinv(pinvRes)) {
                    debug("detection/pinv", true);
                    m_chipDetected = true;
                }
            }
            if (m_chipDetected) {
                parabolicFlightReconstruct(pinvRes);
            }
        }
    }

    if (m_kickFrames.size() > 30 && !m_chipDetected) {
        debug("abort still no detection", true);
        resetFlightReconstruction();
    }
    if (m_kickFrames.size() >= MAX_FRAMES_PER_FLIGHT) {
        resetFlightReconstruction();
    }

#ifdef ENABLE_TRACKING_DEBUG
    if (!m_wasDetectedBefore && m_chipDetected) {
        std::cout << "chip detected " << m_primaryCamera << " " << m_kickFrames.size() << std::endl;
    }
    m_wasDetectedBefore = m_chipDetected;
#endif
}

FlyFilter::Prediction FlyFilter::predictTrajectory(qint64 time)
{
    Eigen::Vector2f groundPos;
    double zSpeed;
    float zPos;

    double flightDuration = 2*m_zSpeed / 9.81;
    double t = (time - (m_chipStartTime+m_initTime)) / 1000000000.0;

    debug("flight duration", flightDuration);
    debug("flight time passed", t);

    m_touchdownPos = m_chipStartPos + flightDuration*m_groundSpeed;
    debugCircle("chosen pos start", m_chipStartPos(0), m_chipStartPos(1), 0.03);
    if (!std::isnan(m_touchdownPos(0)) && !std::isnan(m_touchdownPos(1))) {
        debugCircle("touchdown", m_touchdownPos(0), m_touchdownPos(1), 0.04);
    }

    bool abortBounce = false;
    if (m_isActive && t > 0.3 && t < 3 && t > flightDuration) { // check for bouncing
        if (!m_bouncing) {
            m_bounceStartTime = m_chipStartTime + flightDuration*1000000000;
            m_bounceZSpeed = floorDamping * m_zSpeed;
            m_bounceGroundSpeed = m_groundSpeed;
            m_bouncing = true;
            m_bounceStartPos = m_touchdownPos;
        } else {
            float bounceFlightDuration = 2*m_bounceZSpeed / 9.81;
            double bounceTime = (time - (m_bounceStartTime+m_initTime)) / 1000000000.0;
            debug("bounce/time", bounceTime);
            if (bounceTime > bounceFlightDuration) {
                m_bounceStartTime = m_bounceStartTime + bounceFlightDuration*1000000000;
                m_bounceStartPos = m_bounceStartPos + m_bounceGroundSpeed*bounceFlightDuration;
                m_bounceZSpeed = floorDamping * m_bounceZSpeed;
            }
            debugCircle("bounce start", m_bounceStartPos(0), m_bounceStartPos(1), 0.03);
            // if bounce height below threshold
            float tb = bounceFlightDuration / 2;
            float bounceHeight = m_bounceZSpeed * tb - (9.81/2)*tb*tb;
            debug ("bounce/z speed", m_bounceZSpeed);
            debug ("bounce/flight duration", bounceFlightDuration);
            debug ("bounce/height", bounceHeight);

            if (bounceHeight < 0.01) {
                abortBounce = true;
            }
        }
    } else if (m_isActive && t > flightDuration) {
        debug("abort time over", true);
        //resetFlightReconstruction();
    }

    if (m_bouncing) {
        double groundSpeed = 0;
        int num=0;
        for (int i=m_kickFrames.size()-1; i>0 && i>m_kickFrames.size()-5; i--) {
            auto& fst = m_kickFrames.at(i);
            auto& snd = m_kickFrames.front();
            groundSpeed += (fst.ballPos-snd.ballPos).norm() / (fst.time/1000000000 - snd.time/1000000000);
            num++;
        }
        groundSpeed /= (num+2); // TODO FIXME. error lies probably in m_bounceStartTime
        m_bounceGroundSpeed = m_bounceGroundSpeed.normalized() * groundSpeed;

        debug("bounce/ground speed", m_bounceGroundSpeed.norm());
        float bounceTime = (time - (m_bounceStartTime+m_initTime)) / 1000000000.0;
        groundPos = m_bounceStartPos + m_bounceGroundSpeed.normalized()*groundSpeed * bounceTime;

        zSpeed = m_bounceZSpeed - 9.81*bounceTime;
        zPos = bounceTime * m_bounceZSpeed - 0.5*9.81*bounceTime*bounceTime;
        debug("bounce/zSpeed", zSpeed);
        debug("bounce/zPos", zPos);
        if (abortBounce || zPos < 0 ) {
            debug("abort bounce", true);
            resetFlightReconstruction();
        }
    } else {
        groundPos = m_chipStartPos + m_groundSpeed * t;
        zSpeed = m_zSpeed - 9.81*t;
        zPos = t * m_zSpeed - 0.5*9.81*t*t;
    }

    m_lastPredictionTime = time;
    return Prediction(groundPos(0), groundPos(1), zPos,
        m_groundSpeed(0), m_groundSpeed(1), zSpeed);
}

bool FlyFilter::acceptDetection(const VisionFrame& frame)
{
    // acceptance depends on prediction which makes no sense when not active
    //   for activation of the filter the acceptance is not necessary
    // as the ground filter will accept a ball lying at the ground
    if (!m_isActive) {
        return false;
    }
    auto predTime = (frame.time < m_lastPredictionTime) ? m_lastPredictionTime : frame.time;
    auto pred = predictTrajectory(predTime);
    Eigen::Vector3f cam = m_cameraInfo->cameraPosition.value(frame.cameraId);
    float lambda = -cam(2) / (cam(2)-pred.pos(2));
    Eigen::Vector3f predGround = cam + (cam-pred.pos)*lambda;
    Eigen::Vector3f ball(frame.x, frame.y, 0);

    debugCircle((QString("cam"+QString::number(m_primaryCamera))).toStdString().c_str(), cam(0), cam(1), 0.04);

    auto acceptDist = (ball - predGround).norm();
    debug("accept dist", acceptDist);
    return acceptDist < ACCEPT_DIST;
}


void FlyFilter::writeBallState(world::Ball *ball, qint64 predictionTime)
{
    const Prediction& p = predictTrajectory(predictionTime);
    // leave prediction to kalman filter for low flying balls
    if ((m_isActive && !m_bouncing) || p.pos(2) > 0.05) {
        ball->set_p_x(p.pos(0));
        ball->set_p_y(p.pos(1));
        ball->set_v_x(p.speed(0));
        ball->set_v_y(p.speed(1));
    } else {
        debug("hybrid filter", true);
    }
    ball->set_p_z(p.pos(2));
    ball->set_v_z(p.speed(2));
    ball->set_is_bouncing(m_bouncing);
    ball->set_touchdown_x(m_touchdownPos(0));
    ball->set_touchdown_y(m_touchdownPos(1));
}

bool FlyFilter::isShot()
{
    if (m_shotDetected) {
        m_shotDetected = false;
        return true;
    }
    return false;
}

void FlyFilter::resetFlightReconstruction()
{
    debug("RESET", true);
    m_isActive = false;
    m_chipDetected = false;
    m_bouncing = false;
    m_kickFrames.clear();
    m_flyFitter.clear();
    m_pinvDataInserted = 0;
    m_d_detailed = Eigen::VectorXf::Zero(2*MAX_FRAMES_PER_FLIGHT);
    m_D_detailed = Eigen::MatrixXf::Zero(2*MAX_FRAMES_PER_FLIGHT, 6);
    m_d_coarseControl = Eigen::VectorXf::Zero(2*MAX_FRAMES_PER_FLIGHT);
    m_D_coarseControl = Eigen::MatrixXf::Zero(2*MAX_FRAMES_PER_FLIGHT, 4);
}
