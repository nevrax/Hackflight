/*
   hackflight.hpp : general header, plus init and update methods

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Hackflight is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MEReceiverHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cmath>

#include "board.hpp"
#include "mixer.hpp"
#include "model.hpp"
#include "msp.hpp"
#include "receiver.hpp"
#include "stabilize.hpp"
#include "altitude.hpp"
#include "timedtask.hpp"
#include "model.hpp"
#include "debug.hpp"

namespace hf {

class Hackflight {

    private: // constants

        // Loop timing
        const uint32_t imuLoopMicro       = 3500;
        const uint32_t rcLoopMilli        = 10;
        const uint32_t altHoldLoopMilli   = 25;
        const uint32_t angleCheckMilli    = 500;

        const uint32_t delayMilli         = 100;
        const uint32_t ledFlashMilli      = 1000;
        const uint32_t ledFlashCount      = 20;

    public:

        void init(Board * _board, Receiver *_receiver, Model * _model);
        void update(void);

    private:

        void flashLed(void);
        void outer(void);
        void inner(void);
        void checkAngle(void);

        Mixer      mixer;
        MSP        msp;
        Stabilize  stab;
        Altitude   alti;

        Board    * board;
        Receiver * receiver;

        TimedTask innerTask;
        TimedTask outerTask;
        TimedTask angleCheckTask;
        TimedTask altitudeTask;

        bool     armed;
        bool     failsafe;
        float    yawInitial;
        uint8_t  auxState;
        float    eulerAngles[3];
        bool     safeToArm;
};

/********************************************* CPP ********************************************************/

void Hackflight::init(Board * _board, Receiver * _receiver, Model * _model)
{  
    board = _board;
    receiver = _receiver;

    // Do hardware initialization for board
    board->init();

    // Flash the LEDs to indicate startup
    flashLed();

    // Sleep  a bit to allow IMU to catch up
    board->delayMilliseconds(delayMilli);

    // Initialize essential timing tasks
    innerTask.init(imuLoopMicro);
    outerTask.init(rcLoopMilli * 1000);
    angleCheckTask.init(angleCheckMilli * 1000);

    // Initialize the receiver
    receiver->init();

    // Initialize our stabilization, mixing, and MSP (serial comms)
    stab.init(_model);
    mixer.init(board); 
    msp.init(&mixer, receiver, board);

    // Initialize altitude estimator, which will be used if there's a barometer
    altitudeTask.init(altHoldLoopMilli * 1000);
    alti.init(board, _model);

    // Start unarmed
    armed = false;
    safeToArm = false;
    failsafe = false;

} // init

void Hackflight::update(void)
{
    // Grab current time for various loops
    uint32_t currentTime = (uint32_t)board->getMicros();

    // Outer (slow) loop: respond to receiver demands
    if (outerTask.checkAndUpdate(currentTime)) {
        outer();
    }

    // Altithude-PID task (never called in same loop iteration as Receiver update)
    else if (altitudeTask.checkAndUpdate(currentTime)) {
        alti.computePid(armed);
    }

    // Inner (fast) loop: stabilize, spin motors
    if (innerTask.checkAndUpdate(currentTime)) {
        inner();
    }

    // Periodically check pitch, roll angle for arming readiness
    if (angleCheckTask.ready(currentTime)) {
        checkAngle();
    }

    // Failsafe
    if (armed && receiver->lostSignal()) {
        mixer.cutMotors();
        armed = false;
        failsafe = true;
        board->ledSet(false);
    }

} // update

void Hackflight::outer(void)
{
    // Update Receiver channels
    receiver->update();

    // When landed, reset integral component of PID
    if (receiver->throttleIsDown()) {
        stab.resetIntegral();
    }

    // Certain actions (arming, disarming) need checking every time
    if (receiver->changed()) {

        // actions during armed
        if (armed) {      

            // Disarm
            if (receiver->disarming()) {
                if (armed) {
                    armed = false;
                }
            }

        // Actions during not armed
        } else {         

            // Arming
            if (receiver->arming()) {
    
                if (!failsafe && safeToArm) {

                    auxState = receiver->getAuxState();

                    if (!auxState) // aux switch must be in zero position
                        if (!armed) {
                            yawInitial = eulerAngles[AXIS_YAW];
                            armed = true;
                        }
                }
            }

        } // not armed

    } // receiver->changed()

    // Detect aux switch changes for altitude-hold, loiter, etc.
    if (receiver->getAuxState() != auxState) {
        auxState = receiver->getAuxState();
        alti.handleAuxSwitch(auxState, receiver->demands[Receiver::DEMAND_THROTTLE]);
    }
}

void Hackflight::inner(void)
{
    // Compute exponential Receiver commands, passing yaw angle for headless mode
    receiver->computeExpo(eulerAngles[AXIS_YAW] - yawInitial);

    // Get Euler angles and raw gyro from board
    float gyroRadiansPerSecond[3];
    board->getImu(eulerAngles, gyroRadiansPerSecond);

    // Convert heading from [-pi,+pi] to [0,2*pi]
    if (eulerAngles[AXIS_YAW] < 0) {
        eulerAngles[AXIS_YAW] += 2*M_PI;
    }

    // Set LED based on arming status
    board->ledSet(armed);

    // Udate altitude with accelerometer data
    // XXX Should be done in hardware!
    alti.fuseWithImu(eulerAngles, armed);

    // Modify demands based on extras (currently just altitude-hold)
    alti.modifyDemand(receiver->demands[Receiver::DEMAND_THROTTLE]);

    // Stabilization is synced to IMU update.  Stabilizer also uses RC demands and raw gyro values. 
    stab.update(receiver->demands, eulerAngles, gyroRadiansPerSecond);

    // Support motor testing from GCS
    if (!armed) {
        mixer.runDisarmed();
    }

    // Update mixer (spin motors) unless failsafe triggered or currently arming via throttle-down
    else if (!failsafe && !receiver->throttleIsDown()) {
        mixer.runArmed(receiver->demands[Receiver::DEMAND_THROTTLE], stab.pidRoll, stab.pidPitch, stab.pidYaw);
    }

    // Cut motors on failsafe or throttle-down
    else {
        mixer.cutMotors();
    }

    // Update serial comms
    msp.update(eulerAngles, armed);
} 

void Hackflight::checkAngle(void)
{
    safeToArm = fabs(eulerAngles[AXIS_ROLL])  < stab.maxArmingAngle && fabs(eulerAngles[AXIS_PITCH]) < stab.maxArmingAngle;
}

void Hackflight::flashLed(void)
{
    uint32_t pauseMilli = ledFlashMilli / ledFlashCount;
    board->ledSet(false);
    for (uint8_t i = 0; i < ledFlashCount; i++) {
        board->ledSet(true);
        board->delayMilliseconds(pauseMilli);
        board->ledSet(false);
        board->delayMilliseconds(pauseMilli);
    }
    board->ledSet(false);
}

} // namespace
