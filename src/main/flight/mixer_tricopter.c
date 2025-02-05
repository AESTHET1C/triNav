/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <float.h>
#include "build/debug.h"

#include "common/filter.h"
#include "common/maths.h"

#include "config/config_master.h"
#include "config/parameter_group_ids.h"

#include "drivers/time.h"

#include "fc/fc_core.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"

#include "flight/mixer.h"
#include "flight/mixer_tricopter.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "io/beeper.h"

#include "sensors/gyro.h"

#define IsDelayElapsed_us(timestamp_us, delay_us) ((uint32_t)(micros() - timestamp_us) >= delay_us)
#define IsDelayElapsed_ms(timestamp_ms, delay_ms) ((uint32_t)(millis() - timestamp_ms) >= delay_ms)

PG_REGISTER_WITH_RESET_TEMPLATE(triflightConfig_t, triflightConfig, PG_TRIFLIGHT_CONFIG, 0);

PG_RESET_TEMPLATE(triflightConfig_t, triflightConfig,
    .tri_dynamic_yaw_minthrottle   = 100,
    .tri_dynamic_yaw_maxthrottle   = 100,
    .tri_dynamic_yaw_hoverthrottle = 0,
    .tri_motor_acc_yaw_correction  = 6,
    .tri_motor_acceleration        = 18,
    .tri_servo_angle_at_max        = 400,
    .tri_servo_feedback            = TRI_SERVO_FB_RSSI,
	.tri_servo_direction           = TRI_SERVO_DIRECTION_NORMAL,
    .tri_servo_max_adc             = 0,
    .tri_servo_mid_adc             = 0,
    .tri_servo_min_adc             = 0,
    .tri_tail_motor_index          = 0,
    .tri_tail_motor_thrustfactor   = 138,
    .tri_tail_servo_speed          = 300,
);

static tailTune_t tailTune = {.mode = TT_MODE_NONE};
static uint16_t tailServoADCValue    = 0;

static int16_t  tailMotorAccelerationDelay_angle;
static int16_t  tailMotorDecelerationDelay_angle;
static int16_t  tailMotorPitchZeroAngle;
static uint16_t tailServoAngle       = TRI_TAIL_SERVO_ANGLE_MID;
static uint8_t  tailServoDirection   = TRI_SERVO_DIRECTION_NORMAL;
static int32_t  tailServoMaxYawForce = 0;
static int16_t  tailServoMaxAngle    = 0;
static int16_t  tailServoSpeed       = 0;
static int32_t  yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE];

int32_t         hoverThrottleSum;
float           tailServoThrustFactor = 0;

// Virtual tail motor speed feedback
static float tailMotorVirtual = 1000.0f;

// Configured output throttle range (max - min)
static int16_t throttleRange = 0;

// Motor acceleration in output units (us) / second
static float motorAcceleration = 0;

// Reset the I term when tail motor deceleration has lasted (ms)
static uint16_t resetITermDecelerationLasted_ms = 0;

static int16_t      * gpTailServo;
static servoParam_t * gpTailServoConf;

static adcFunction_e tailServoADCChannel = ADC_RSSI;

static int16_t  dynamicYaw(int16_t PIDoutput);
static uint16_t feedbackServoStep(uint16_t tailServoADCValue);
static uint16_t getAngleFromYawCurveAtForce(int32_t force);
static uint16_t getLinearServoValue(servoParam_t *servoConf, int16_t constrainedPIDOutput);
static float    getPitchCorrectionAtTailAngle(float angle, float thrustFactor);
static uint16_t getPitchCorrectionMaxPhaseShift(int16_t servoAngle,
                                                int16_t servoSetpointAngle,
                                                int16_t motorAccelerationDelayAngle,
                                                int16_t motorDecelerationDelayAngle,
                                                int16_t motorDirectionChangeAngle);
static uint16_t getServoAngle(servoParam_t * servoConf, uint16_t servoValue);
static uint16_t getServoValueAtAngle(servoParam_t * servoConf, uint16_t angle);
static void     initCurves(void);
static uint16_t virtualServoStep(uint16_t currentAngle, int16_t servoSpeed, float dT, servoParam_t *servoConf, uint16_t servoValue);
static void     virtualMotorStep(int16_t setpoint, float dT);
static void     tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal, float dT);
static void     tailTuneHandler(servoParam_t *pServoConf, int16_t *pServoVal, float dT);
static void     tailTuneModeThrustTorque(thrustTorque_t *pTTR, const bool isThrottleHigh);
static void     updateServoAngle(float dT);

static pt1Filter_t feedbackFilter;
static pt1Filter_t motorFilter;

void triMixerInit(servoParam_t *pTailServoConfig, int16_t *pTailServo)
{
    gpTailServoConf       = pTailServoConfig;
    gpTailServo           = pTailServo;
    tailServoDirection    = triflightConfig()->tri_servo_direction;
    tailServoThrustFactor = triflightConfig()->tri_tail_motor_thrustfactor / 10.0f;
    tailServoMaxAngle     = triflightConfig()->tri_servo_angle_at_max;
    tailServoSpeed        = triflightConfig()->tri_tail_servo_speed;

    throttleRange         = motorConfig()->maxthrottle - getThrottleIdleValue();
    motorAcceleration     = (float)throttleRange / ((float)triflightConfig()->tri_motor_acceleration * 0.01f);

    // Reset the I term when motor deceleration has lasted 35% of the min to max time
    resetITermDecelerationLasted_ms = (float)triflightConfig()->tri_motor_acceleration * 10.0f * 0.35f;

    // Configure ADC data source
    switch (triflightConfig()->tri_servo_feedback) {
    default:
    case TRI_SERVO_FB_RSSI:
        tailServoADCChannel = ADC_RSSI;
        break;
    case TRI_SERVO_FB_CURRENT:
        tailServoADCChannel = ADC_CURRENT;
        break;
    }

    initCurves();
}

static void initCurves(void)
{
    // DERIVATE(1/(sin(x)-cos(x)/tailServoThrustFactor)) = 0
    // Multiplied by 10 to get decidegrees
    tailMotorPitchZeroAngle = 10.0f * 2.0f * (atanf(((sqrtf(tailServoThrustFactor * tailServoThrustFactor + 1) + 1) / tailServoThrustFactor)));

    tailMotorAccelerationDelay_angle = 10.0f * (TRI_MOTOR_ACCELERATION_DELAY_MS / 1000.0f) * tailServoSpeed;
    tailMotorDecelerationDelay_angle = 10.0f * (TRI_MOTOR_DECELERATION_DELAY_MS / 1000.0f) * tailServoSpeed;

    const int16_t minAngle = TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle;
    const int16_t maxAngle = TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle;
    int32_t maxNegForce    = 0;
    int32_t maxPosForce    = 0;

    int16_t angle = TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    for (int32_t i = 0; i < TRI_YAW_FORCE_CURVE_SIZE; i++)
    {
        const float angleRad = DEGREES_TO_RADIANS(angle / 10.0f);
        yawForceCurve[i] = TRI_YAW_FORCE_PRECISION * (-tailServoThrustFactor * cosf(angleRad) - sinf(angleRad)) * getPitchCorrectionAtTailAngle(angleRad, tailServoThrustFactor);
        // Only calculate the top forces in the configured angle range
        if ((angle >= minAngle) && (angle <= maxAngle))
        {
            maxNegForce = MIN(yawForceCurve[i], maxNegForce);
            maxPosForce = MAX(yawForceCurve[i], maxPosForce);
        }
        angle += 10;
    }

    tailServoMaxYawForce = MIN(ABS(maxNegForce), ABS(maxPosForce));
}

uint16_t triGetCurrentServoAngle(void)
{
    return tailServoAngle;
}

static uint16_t getLinearServoValue(servoParam_t *servoConf, int16_t constrainedPIDOutput)
{
    const int32_t linearYawForceAtValue = tailServoMaxYawForce * constrainedPIDOutput / TRI_YAW_FORCE_PRECISION;
    const int16_t correctedAngle = getAngleFromYawCurveAtForce(linearYawForceAtValue);
    
    return getServoValueAtAngle(servoConf, correctedAngle);
}

void triServoMixer(int16_t PIDoutput, float dT)
{
    // Scale the PID output based on tail motor speed (thrust)
    PIDoutput = dynamicYaw(constrain(PIDoutput, -1000, 1000));

    if (triflightConfig()->tri_servo_feedback != TRI_SERVO_FB_VIRTUAL)
    {
        // Read new servo feedback signal sample and run it through filter
        tailServoADCValue = pt1FilterApply4(&feedbackFilter,
                                       adcGetChannel(tailServoADCChannel),
                                       TRI_SERVO_FEEDBACK_LPF_CUTOFF_HZ,
                                       dT);
    }

    updateServoAngle(dT);

	*gpTailServo = getLinearServoValue(gpTailServoConf, PIDoutput);

    // Debug
    DEBUG_SET(DEBUG_TRIFLIGHT, 0, (uint32_t)adcGetChannel(tailServoADCChannel));
    DEBUG_SET(DEBUG_TRIFLIGHT, 1, (uint32_t)tailServoADCValue);
    DEBUG_SET(DEBUG_TRIFLIGHT, 2, (uint32_t)tailServoAngle);

    tailTuneHandler(gpTailServoConf, gpTailServo, dT);

    // Update the tail motor virtual feedback
    virtualMotorStep(motor[triflightConfig()->tri_tail_motor_index], dT);
}

int16_t triGetMotorCorrection(uint8_t motorIndex)
{
    uint16_t correction = 0;
    if (motorIndex == triflightConfig()->tri_tail_motor_index)
    {
        // Adjust tail motor speed based on servo angle. Check how much to adjust speed from pitch force curve based on servo angle.
        // Take motor speed up lag into account by shifting the phase of the curve
        // Not taking into account the motor braking lag (yet)
        const uint16_t servoAngle = triGetCurrentServoAngle();
        const uint16_t servoSetpointAngle = getServoAngle(gpTailServoConf, *gpTailServo);

        const uint16_t maxPhaseShift = getPitchCorrectionMaxPhaseShift(servoAngle, servoSetpointAngle, tailMotorAccelerationDelay_angle, tailMotorDecelerationDelay_angle, tailMotorPitchZeroAngle);

        int16_t angleDiff = servoSetpointAngle - servoAngle;
        if (ABS(angleDiff) > maxPhaseShift)
        {
            angleDiff = (int32_t)maxPhaseShift * angleDiff / ABS(angleDiff);
        }

        const int16_t futureServoAngle = constrain(servoAngle + angleDiff, TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle, TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle);
        uint16_t throttleMotorOutput = tailMotorVirtual - getThrottleIdleValue();
        /* Increased yaw authority at min throttle, always calculate the pitch
         * correction on at least half motor output. This produces a little bit
         * more forward pitch, but tested to be negligible.
         *
         * TODO: this is not the best way to achieve this, but how could the min_throttle
         * pitch correction be calculated, as the thrust is zero?
         */
        throttleMotorOutput = constrain(throttleMotorOutput, throttleRange / 2, 1000);
        correction = (throttleMotorOutput * getPitchCorrectionAtTailAngle(DEGREES_TO_RADIANS(futureServoAngle / 10.0f), tailServoThrustFactor)) - throttleMotorOutput;
    }

    return correction;
}

static uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle)
{
    const int16_t servoMin = servoConf->min;
    const int16_t servoMid = servoConf->middle;
    const int16_t servoMax = servoConf->max;

    uint16_t servoValue;

    if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL)
    {
        if (angle < TRI_TAIL_SERVO_ANGLE_MID)
        {
            servoValue = (int32_t)(angle - (TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle)) * (servoMid - servoMin) / 
                                  (TRI_TAIL_SERVO_ANGLE_MID - (TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle)) + servoMin;
        }
        else if (angle > TRI_TAIL_SERVO_ANGLE_MID)
        {
            servoValue = (int32_t)(angle - TRI_TAIL_SERVO_ANGLE_MID) * (servoMax - servoMid) / tailServoMaxAngle + servoMid;
        }
        else
        {
            servoValue = servoMid;
        }
    }
    else
    {
        if (angle < TRI_TAIL_SERVO_ANGLE_MID)
        {
            servoValue = servoMax - (int32_t)(angle - (TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle)) * (servoMid - servoMin) / 
                         (TRI_TAIL_SERVO_ANGLE_MID - (TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle));
        }
        else if (angle > TRI_TAIL_SERVO_ANGLE_MID)
        {
            servoValue = servoMid - (int32_t)(angle - TRI_TAIL_SERVO_ANGLE_MID) * (servoMax - servoMid) / tailServoMaxAngle;
        }
        else
        {
            servoValue = servoMid;
        }
    }

    return servoValue;
}

static float getPitchCorrectionAtTailAngle(float angle, float thrustFactor)
{
    return 1 / (sin_approx(angle) - cos_approx(angle) / thrustFactor);
}

static uint16_t getAngleFromYawCurveAtForce(int32_t force)
{
    if (force < yawForceCurve[0]) // No force that low
    {
        return TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    }
    else if (!(force < yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE - 1])) // No force that high
    {
        return TRI_TAIL_SERVO_ANGLE_MID + TRI_TAIL_SERVO_MAX_ANGLE;
    }
    // Binary search: yawForceCurve[lower] <= force, yawForceCurve[higher] > force
    int32_t lower = 0, higher = TRI_YAW_FORCE_CURVE_SIZE - 1;
    while (higher > lower + 1)
    {
        const int32_t mid = (lower + higher) / 2;
        if (yawForceCurve[mid] > force)
        {
            higher = mid;
        }
        else
        {
            lower = mid;
        }
    }
    // Interpolating
    return TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE + lower * 10 + (int32_t)(force - yawForceCurve[lower]) * 10 / (yawForceCurve[higher] - yawForceCurve[lower]);
}

static uint16_t getServoAngle(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t midValue   = servoConf->middle;

    const int16_t endValue   = servoValue < midValue ? servoConf->min : servoConf->max;

    const int16_t endAngle   = servoValue < midValue ? -tailServoMaxAngle : tailServoMaxAngle;

    int16_t servoAngle;

    if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL)
        servoAngle = (int32_t)(endAngle) * (servoValue - midValue) / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID;
    else
        servoAngle = TRI_TAIL_SERVO_ANGLE_MID - (int32_t)(endAngle) * (servoValue - midValue) / (endValue - midValue);
    
    return servoAngle;
}

static uint16_t getPitchCorrectionMaxPhaseShift(int16_t servoAngle,
                                                int16_t servoSetpointAngle,
                                                int16_t motorAccelerationDelayAngle,
                                                int16_t motorDecelerationDelayAngle,
                                                int16_t motorDirectionChangeAngle)
{
    uint16_t maxPhaseShift;

    if (((servoAngle > servoSetpointAngle) && (servoAngle >= (motorDirectionChangeAngle + motorAccelerationDelayAngle))) ||
        ((servoAngle < servoSetpointAngle) && (servoAngle <= (motorDirectionChangeAngle - motorAccelerationDelayAngle))))
    {
        // Motor is braking
        maxPhaseShift = ABS(servoAngle - motorDirectionChangeAngle) >= motorDecelerationDelayAngle ?
                                                                                                   motorDecelerationDelayAngle:
                                                                                                   ABS(servoAngle - motorDirectionChangeAngle);
    }
    else
    {
        // Motor is accelerating
        maxPhaseShift = motorAccelerationDelayAngle;
    }

    return maxPhaseShift;
}

static uint16_t virtualServoStep(uint16_t currentAngle, int16_t servoSpeed, float dT, servoParam_t *servoConf, uint16_t servoValue)
{
    const uint16_t angleSetPoint = getServoAngle(servoConf, servoValue);
    const uint16_t dA            = dT * servoSpeed * 10; // Max change of an angle since last check

    if ( ABS(currentAngle - angleSetPoint) < dA )
    {
        // At set-point after this moment
        currentAngle = angleSetPoint;
    }
    else if (currentAngle < angleSetPoint)
    {
        currentAngle += dA;
    }
    else // tailServoAngle.virtual > angleSetPoint
    {
        currentAngle -= dA;
    }

    return currentAngle;
}

static uint16_t feedbackServoStep(uint16_t tailServoADCValue)
{
    uint16_t feedbackAngle;

    // Feedback servo
    const int32_t ADCFeedback       = tailServoADCValue;
    const int16_t midValue          = triflightConfig()->tri_servo_mid_adc;
	
    const int16_t endValue          = ADCFeedback < midValue ? triflightConfig()->tri_servo_min_adc : triflightConfig()->tri_servo_max_adc;
	
    const int16_t endAngle          = ADCFeedback < midValue ? -tailServoMaxAngle : tailServoMaxAngle;

    if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL)
	    feedbackAngle = endAngle * (ADCFeedback - midValue) / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID;
    else
	    feedbackAngle =  TRI_TAIL_SERVO_ANGLE_MID - endAngle * (ADCFeedback - midValue) / (endValue - midValue);	

    return feedbackAngle;
}

static void updateServoAngle(float dT)
{
    if (triflightConfig()->tri_servo_feedback == TRI_SERVO_FB_VIRTUAL) {
        tailServoAngle = virtualServoStep(tailServoAngle, tailServoSpeed, dT, gpTailServoConf, *gpTailServo);
    } else {
        tailServoAngle = feedbackServoStep(tailServoADCValue);
    }
}

static int16_t dynamicYaw(int16_t PIDoutput)
{
    static int32_t range     = 0;
    static int32_t lowRange  = 0;
    static int32_t highRange = 0;
	
    int16_t gain;

    if (triflightConfig()->tri_dynamic_yaw_hoverthrottle == 0)
        return PIDoutput;

    if (range == 0 && lowRange == 0 && highRange == 0)
    {
        range     = (int32_t)(mixGetMotorOutputHigh() - mixGetMotorOutputLow());
        lowRange  = range * (triflightConfig()->tri_dynamic_yaw_hoverthrottle - (int32_t)mixGetMotorOutputLow()) / range;
        highRange = range - lowRange;
    }

    // Debug
    //DEBUG_SET(DEBUG_TRIFLIGHT, 0, mixGetMotorOutputLow());
    //DEBUG_SET(DEBUG_TRIFLIGHT, 1, mixGetMotorOutputHigh());

    // Debug pt. 2: Debugging Boogaloo
    //DEBUG_SET(DEBUG_TRIFLIGHT, 0, range);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 1, lowRange);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 2, highRange);

    // Select the yaw gain based on tail motor speed
    if (tailMotorVirtual < triflightConfig()->tri_dynamic_yaw_hoverthrottle)
    {
        /* Below hover point, gain is increasing the output.
         * e.g. 150 (%) increases the yaw output at min throttle by 150 % (1.5x)
         * e.g. 250 (%) increases the yaw output at min throttle by 250 % (2.5x)
         */
        gain = triflightConfig()->tri_dynamic_yaw_minthrottle - 100;
    }
    else
    {
        /* Above hover point, gain is decreasing the output.
         * e.g. 75 (%) reduces the yaw output at max throttle by 25 % (0.75x)
         * e.g. 20 (%) reduces the yaw output at max throttle by 80 % (0.2x)
         */
        gain = 100 - triflightConfig()->tri_dynamic_yaw_maxthrottle;
    }

    int16_t distanceFromMid = (tailMotorVirtual - triflightConfig()->tri_dynamic_yaw_hoverthrottle);

    int32_t scaledPIDoutput;

    if (lowRange == 0 || highRange == 0)
    scaledPIDoutput = PIDoutput;
    else
    {
        if (tailMotorVirtual < triflightConfig()->tri_dynamic_yaw_hoverthrottle)
            scaledPIDoutput = PIDoutput - distanceFromMid * gain * PIDoutput / (lowRange * 100);
        else
            scaledPIDoutput = PIDoutput - distanceFromMid * gain * PIDoutput / (highRange * 100);
    }

    // Debug
    //DEBUG_SET(DEBUG_TRIFLIGHT, 0, tailMotorVirtual);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 1, gain);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 2, PIDoutput);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 3, scaledPIDoutput);

	return constrain(scaledPIDoutput, -1000, 1000);
}

static void virtualMotorStep(int16_t setpoint, float dT)
{
    static float current = 1000;
    float dS; // Max change of an speed since last check

    dS = dT * motorAcceleration;

    if ( ABS(current - setpoint) < dS )
    {
        // At set-point after this moment
        current = setpoint;
    }
    else if (current < setpoint)
    {
        current += dS;
    }
    else
    {
        current -= dS;
    }

    // Apply low-pass filter to the virtual motor feedback
    // Cut-off to delay:
    // 2  Hz -> 25 ms
    // 5  Hz -> 14 ms
    // 10 Hz -> 9  ms
    //

    tailMotorVirtual = pt1FilterApply4(&motorFilter, 
                                       current,
                                       TRI_MOTOR_FEEDBACK_LPF_CUTOFF_HZ,
                                       dT);
 
    // Debug
    //DEBUG_SET(DEBUG_TRIFLIGHT, 0, setpoint);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 1, current);
    //DEBUG_SET(DEBUG_TRIFLIGHT, 2, tailMotorVirtual);

}

///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////

bool isRcAxisWithinDeadband(int32_t axis)
{
    int32_t tmp = MIN(ABS(rcCommand[axis]), 500);
    bool ret = false;
    if (axis == ROLL || axis == PITCH) {
        if (tmp <= rcControlsConfig()->deadband)
            ret = true;
    } else if (tmp <= rcControlsConfig()->yaw_deadband)
        ret = true;

    return ret;
}

static void tailTuneHandler(servoParam_t *pServoConf, int16_t *pServoVal, float dT)
{
    // Enable or disable TailTune flight mode
    // TODO: should this be moved to an init function?
    if (!IS_RC_MODE_ACTIVE(BOXTAILTUNE)) {
        if (FLIGHT_MODE(TAILTUNE_MODE)) {
            DISABLE_ARMING_FLAG(ARMING_DISABLED_TAILTUNE);
            DISABLE_FLIGHT_MODE(TAILTUNE_MODE);
            tailTune.mode = TT_MODE_NONE;
        }
        return;
    } else
        ENABLE_FLIGHT_MODE(TAILTUNE_MODE);

    // Select TailTune mode if not already active
    if (tailTune.mode == TT_MODE_NONE) {
        if (ARMING_FLAG(ARMED)) {
            tailTune.mode     = TT_MODE_THRUST_TORQUE;
            tailTune.ttr.state = TTR_IDLE;
        } else {
            // Prevent accidental arming in servo setup mode
            ENABLE_ARMING_FLAG(ARMING_DISABLED_TAILTUNE);
            
            tailTune.mode        = TT_MODE_SERVO_SETUP;
            tailTune.ss.servoVal = pServoConf->middle;
        }
    }

    switch (tailTune.mode) {
        case TT_MODE_THRUST_TORQUE:
            tailTuneModeThrustTorque(&tailTune.ttr, (THROTTLE_HIGH == calculateThrottleStatus(THROTTLE_STATUS_TYPE_RC)));
            break;
        case TT_MODE_SERVO_SETUP:
            tailTuneModeServoSetup(&tailTune.ss, pServoConf, pServoVal, dT);
            break;
        default:
        case TT_MODE_NONE:
            break;
    }
}

static void tailTuneModeThrustTorque(thrustTorque_t *pTTR, const bool isThrottleHigh)
{
    switch(pTTR->state) {
        case TTR_IDLE:
            // Calibration has been requested, only start when throttle is up
            if (isThrottleHigh && ARMING_FLAG(ARMED)) {
                beeper(BEEPER_BAT_LOW);

                pTTR->startBeepDelay_ms   = 1000;
                pTTR->timestamp_ms        = millis();
                pTTR->lastAdjTime_ms      = millis();
                pTTR->state               = TTR_WAIT;
                pTTR->servoAvgAngle.sum   = 0;
                pTTR->servoAvgAngle.numOf = 0;
                hoverThrottleSum          = 0;
            }
            break;

        case TTR_WAIT:
            if (isThrottleHigh && ARMING_FLAG(ARMED)) {
                /* Wait for 5 seconds before activating the tuning.
                This is so that pilot has time to take off if the tail tune mode was activated on ground. */
                if (IsDelayElapsed_ms(pTTR->timestamp_ms, 5000)) {
                    // Longer beep when starting
                    beeper(BEEPER_BAT_CRIT_LOW);

                    pTTR->state        = TTR_ACTIVE;
                    pTTR->timestamp_ms = millis();
                } else if (IsDelayElapsed_ms(pTTR->timestamp_ms, pTTR->startBeepDelay_ms)) {
                    // Beep every second until start
                    beeper(BEEPER_BAT_LOW);

                    pTTR->startBeepDelay_ms += 1000;
                }
            } else
                pTTR->state = TTR_IDLE;
            break;

        case TTR_ACTIVE:
            if (isThrottleHigh &&
            isRcAxisWithinDeadband(ROLL)  &&
            isRcAxisWithinDeadband(PITCH) &&
            isRcAxisWithinDeadband(YAW)   &&
            (fabsf(gyro.gyroADCf[FD_YAW]) <= 10.0f)) {  // deg/s
                if (IsDelayElapsed_ms(pTTR->timestamp_ms, 250)) {
                    // RC commands have been within deadbands for 250 ms
                    if (IsDelayElapsed_ms(pTTR->lastAdjTime_ms, 10)) {
                        pTTR->lastAdjTime_ms = millis();

                        pTTR->servoAvgAngle.sum += triGetCurrentServoAngle();
                        pTTR->servoAvgAngle.numOf++;

                        hoverThrottleSum += (motor[triflightConfig()->tri_tail_motor_index]);

                        beeperConfirmationBeeps(1);

                        if (pTTR->servoAvgAngle.numOf >= 300) {
                            beeper(BEEPER_READY_BEEP);

                            pTTR->state        = TTR_WAIT_FOR_DISARM;
                            pTTR->timestamp_ms = millis();
                        }
                    }
                }
            } else
                pTTR->timestamp_ms = millis();
            break;

        case TTR_WAIT_FOR_DISARM:
            if (!ARMING_FLAG(ARMED)) {
                float averageServoAngle = pTTR->servoAvgAngle.sum / 10.0f / pTTR->servoAvgAngle.numOf;

                if (averageServoAngle > 90.5f && averageServoAngle < 120.f) {
                    averageServoAngle -= 90.0f;
                    averageServoAngle *= RAD;

                    triflightConfigMutable()->tri_tail_motor_thrustfactor = 10.0f * cos_approx(averageServoAngle) / sin_approx(averageServoAngle);

                    triflightConfigMutable()->tri_dynamic_yaw_hoverthrottle = hoverThrottleSum / (int16_t)pTTR->servoAvgAngle.numOf;

                    saveConfigAndNotify();

                    pTTR->state = TTR_DONE;
                } else
                    pTTR->state = TTR_FAIL;
                pTTR->timestamp_ms = millis();
            } else {
                if (IsDelayElapsed_ms(pTTR->timestamp_ms, 2000)) {
                    beeper(BEEPER_READY_BEEP);

                    pTTR->timestamp_ms = millis();
                }
            }
            break;

        case TTR_DONE:
            if (IsDelayElapsed_ms(pTTR->timestamp_ms, 2000)) {
                beeper(BEEPER_ACTION_SUCCESS);

                pTTR->timestamp_ms = millis();
            }
            break;

        case TTR_FAIL:
            if (IsDelayElapsed_ms(pTTR->timestamp_ms, 2000)) {
                beeper(BEEPER_ACTION_FAIL);

                pTTR->timestamp_ms = millis();
            }
            break;
    }
}

// TODO: Figure out literally any other way to do this than 3 layers of nested switches
static void tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal, float dT)
{
    // Check mode select
    if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] < -100)) {
        if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL) {
            pSS->servoVal       = pServoConf->min;
            pSS->pLimitToAdjust = &pServoConf->min;
        } else {
            pSS->servoVal       = pServoConf->max;
            pSS->pLimitToAdjust = &pServoConf->max;
        }

        pSS->state = SS_SETUP;

        beeperConfirmationBeeps(1);
    } else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] > 100)) {
        pSS->servoVal       = pServoConf->middle;
        pSS->pLimitToAdjust = &pServoConf->middle;
        pSS->state          = SS_SETUP;
        
        beeperConfirmationBeeps(2);
    } else if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] > 100)) {
        if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL) {
            pSS->servoVal       = pServoConf->max;
            pSS->pLimitToAdjust = &pServoConf->max;
        } else {
            pSS->servoVal       = pServoConf->min;
            pSS->pLimitToAdjust = &pServoConf->min;
        }

        pSS->state = SS_SETUP;
        
        beeperConfirmationBeeps(3);
    } else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] < -100)) {
        pSS->state     = SS_CALIB;
        pSS->cal.state = SS_C_IDLE;
    }

    switch (pSS->state) {
        case SS_IDLE:
            break;

        case SS_SETUP:
            if (!isRcAxisWithinDeadband(YAW)) {
                if (tailServoDirection == TRI_SERVO_DIRECTION_NORMAL)
                    pSS->servoVal += -1.0f * (float)rcCommand[YAW] * dT;
                else
                    pSS->servoVal +=  1.0f * (float)rcCommand[YAW] * dT;

                constrain(pSS->servoVal, 950, 2050);

                *pSS->pLimitToAdjust = pSS->servoVal;
            }
            break;

        case SS_CALIB:
            // State transition
            if ((pSS->cal.done == true) || (pSS->cal.state == SS_C_IDLE)) {
                if (pSS->cal.state == SS_C_IDLE) {
                    pSS->cal.state            = SS_C_CALIB_MIN_MID_MAX;
                    pSS->cal.subState         = SS_C_MIN;
                    pSS->servoVal             = pServoConf->min;
                    pSS->cal.avg.pCalibConfig = &triflightConfigMutable()->tri_servo_min_adc;
                } else if (pSS->cal.state == SS_C_CALIB_SPEED) {
                    pSS->state = SS_IDLE;
                    pSS->cal.subState = SS_C_MIN;

                    beeper(BEEPER_READY_BEEP);

                    // Speed calibration should be done as final step so this saves the min, mid, max and speed values.
                    saveConfigAndNotify();
                } else {
                    if (pSS->cal.state == SS_C_CALIB_MIN_MID_MAX) {
                        switch (pSS->cal.subState) {
                            case SS_C_MIN:
                                pSS->cal.subState         = SS_C_MID;
                                pSS->servoVal             = pServoConf->middle;
                                pSS->cal.avg.pCalibConfig = &triflightConfigMutable()->tri_servo_mid_adc;
                                break;

                            case SS_C_MID:
                                if (ABS(triflightConfigMutable()->tri_servo_min_adc - triflightConfigMutable()->tri_servo_mid_adc) < 100) {
                                    /* Not enough difference between min and mid feedback values.
                                     * Most likely the feedback signal is not connected.
                                     */
                                    pSS->state        = SS_IDLE;
                                    pSS->cal.subState = SS_C_MIN;

                                    beeper(BEEPER_ACTION_FAIL);

                                    /* Save configuration even after speed calibration failed.
                                     * Speed calibration should be done as final step so this saves the min, mid and max values.
                                     */
                                    saveConfigAndNotify();
                                } else {
                                    pSS->cal.subState         = SS_C_MAX;
                                    pSS->servoVal             = pServoConf->max;
                                    pSS->cal.avg.pCalibConfig = &triflightConfigMutable()->tri_servo_max_adc;
                                }
                                break;

                            case SS_C_MAX:
                                pSS->cal.state              = SS_C_CALIB_SPEED;
                                pSS->cal.subState           = SS_C_MIN;
                                pSS->servoVal               = pServoConf->min;
                                pSS->cal.waitingServoToStop = true;
                                break;
                        }

                        // Debug
                        //DEBUG_SET(DEBUG_TRIFLIGHT, 0, (uint32_t)triflightConfigMutable()->tri_servo_min_adc);
                        //DEBUG_SET(DEBUG_TRIFLIGHT, 1, (uint32_t)triflightConfigMutable()->tri_servo_mid_adc);
                        //DEBUG_SET(DEBUG_TRIFLIGHT, 2, (uint32_t)triflightConfigMutable()->tri_servo_max_adc);
                    }
                }

                pSS->cal.timestamp_ms = millis();
                pSS->cal.avg.sum      = 0;
                pSS->cal.avg.numOf    = 0;
                pSS->cal.done         = false;
            }

            switch (pSS->cal.state) {
                case SS_C_IDLE:
                    break;

                case SS_C_CALIB_MIN_MID_MAX:
                    if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 500)) {
                        if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 600)) {
                            *pSS->cal.avg.pCalibConfig = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                            pSS->cal.done              = true;
                        } else {
                            pSS->cal.avg.sum += tailServoADCValue;
                            pSS->cal.avg.numOf++;
                        }
                    }
                    break;

                case SS_C_CALIB_SPEED:
                    switch (pSS->cal.subState) {
                        case SS_C_MIN:
                            // Wait for the servo to reach min position
                            if (tailServoADCValue < (triflightConfigMutable()->tri_servo_min_adc + 10)) {
                                if (!pSS->cal.waitingServoToStop) {
                                    pSS->cal.avg.sum += millis() - pSS->cal.timestamp_ms;
                                    pSS->cal.avg.numOf++;

                                    if (pSS->cal.avg.numOf > 5) {
                                        const float avgTime       = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                                        const float avgServoSpeed = (2.0f * tailServoMaxAngle / 10.0f) / avgTime * 1000.0f;

                                        triflightConfigMutable()->tri_tail_servo_speed = avgServoSpeed;
                                        tailServoSpeed                                 = triflightConfig()->tri_tail_servo_speed;

                                        pSS->cal.done = true;
                                        pSS->servoVal = pServoConf->middle;
                                    }

                                    pSS->cal.timestamp_ms       = millis();
                                    pSS->cal.waitingServoToStop = true;
                                } else if  (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200)) {  // Wait for the servo to fully stop before starting speed measuring
                                    pSS->cal.timestamp_ms       = millis();
                                    pSS->cal.subState           = SS_C_MAX;
                                    pSS->cal.waitingServoToStop = false;
                                    pSS->servoVal               = pServoConf->max;
                                }
                            }
                            break;

                        case SS_C_MAX:
                            // Wait for the servo to reach max position
                            if (tailServoADCValue > (triflightConfigMutable()->tri_servo_max_adc - 10)) {
                                if (!pSS->cal.waitingServoToStop) {
                                    pSS->cal.avg.sum += millis() - pSS->cal.timestamp_ms;
                                    pSS->cal.avg.numOf++;

                                    pSS->cal.timestamp_ms       = millis();
                                    pSS->cal.waitingServoToStop = true;
                                } else if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200)) {
                                    pSS->cal.timestamp_ms       = millis();
                                    pSS->cal.subState           = SS_C_MIN;
                                    pSS->cal.waitingServoToStop = false;
                                    pSS->servoVal               = pServoConf->min;
                                }
                            }
                            break;

                        case SS_C_MID:
                            // Should not come here
                            break;
                    }
            }
            break;
    }

    *pServoVal = pSS->servoVal;
}
