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
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#include "build/debug.h"
#include "build/build_config.h"

#include "common/axis.h"
#include "common/filter.h"
#include "common/maths.h"
#include "programming/global_variables.h"

#include "config/config_reset.h"
#include "config/feature.h"
#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/pwm_output.h"
#include "drivers/pwm_mapping.h"
#include "drivers/time.h"

#include "fc/config.h"
#include "fc/fc_core.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"
#include "fc/controlrate_profile.h"
#include "fc/settings.h"

#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/mixer_tricopter.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "io/gps.h"

#include "rx/rx.h"

#include "sensors/gyro.h"

PG_REGISTER_WITH_RESET_TEMPLATE(servoConfig_t, servoConfig, PG_SERVO_CONFIG, 3);

PG_RESET_TEMPLATE(servoConfig_t, servoConfig,
    .servoCenterPulse = SETTING_SERVO_CENTER_PULSE_DEFAULT,
    .servoPwmRate = SETTING_SERVO_PWM_RATE_DEFAULT,
    .servo_lowpass_freq = SETTING_SERVO_LPF_HZ_DEFAULT,  // Must be less than half of the PWM rate due to aliasing
    .servo_protocol = SETTING_SERVO_PROTOCOL_DEFAULT,
    .flaperon_throw_offset = SETTING_FLAPERON_THROW_OFFSET_DEFAULT,
    .tri_unarmed_servo = SETTING_TRI_UNARMED_SERVO_DEFAULT,
    .servo_autotrim_rotation_limit = SETTING_SERVO_AUTOTRIM_ROTATION_LIMIT_DEFAULT
);

PG_REGISTER_ARRAY_WITH_RESET_FN(servoMixer_t, MAX_SERVO_RULES, customServoMixers, PG_SERVO_MIXER, 1);

void pgResetFn_customServoMixers(servoMixer_t *instance)
{
    for (int i = 0; i < MAX_SERVO_RULES; i++) {
        RESET_CONFIG(servoMixer_t, &instance[i],
            .targetChannel = 0,
            .inputSource = 0,
            .rate = 0,
            .speed = 0
#ifdef USE_PROGRAMMING_FRAMEWORK
            ,.conditionId = -1
#endif
        );
    }
}

PG_REGISTER_ARRAY_WITH_RESET_FN(servoParam_t, MAX_SUPPORTED_SERVOS, servoParams, PG_SERVO_PARAMS, 3);

void pgResetFn_servoParams(servoParam_t *instance)
{
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        RESET_CONFIG(servoParam_t, &instance[i],
            .min = DEFAULT_SERVO_MIN,
            .max = DEFAULT_SERVO_MAX,
            .middle = DEFAULT_SERVO_MIDDLE,
            .rate = 100
        );        
    }
}

int16_t servo[MAX_SUPPORTED_SERVOS];

static uint8_t servoRuleCount = 0;
static servoMixer_t currentServoMixer[MAX_SERVO_RULES];
static bool servoOutputEnabled;

static bool mixerUsesServos;
static uint8_t minServoIndex;
static uint8_t maxServoIndex;

static biquadFilter_t servoFilter[MAX_SUPPORTED_SERVOS];
static bool servoFilterIsSet;

static servoMetadata_t servoMetadata[MAX_SUPPORTED_SERVOS];
static rateLimitFilter_t servoSpeedLimitFilter[MAX_SERVO_RULES];

STATIC_FASTRAM pt1Filter_t rotRateFilter;
STATIC_FASTRAM pt1Filter_t targetRateFilter;

int16_t getFlaperonDirection(uint8_t servoPin)
{
    return ((servoPin == SERVO_FLAPPERON_2) ? -1 : 1);
}

// Computes the scaling factors for upper and lower servo throws
// Expected values are likely below 1.0
void servoComputeScalingFactors(uint8_t servoIndex) {
    servoMetadata[servoIndex].scaleMax = (servoParams(servoIndex)->max - servoParams(servoIndex)->middle) / 500.0f;
    servoMetadata[servoIndex].scaleMin = (servoParams(servoIndex)->middle - servoParams(servoIndex)->min) / 500.0f;
}

void servosInit(void)
{
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++)
        servo[i] = servoParams(i)->middle;

    loadCustomServoMixer();

    // If servo rules exist, enable servo mixer
    if (servoRuleCount > 0) {
        servoOutputEnabled = true;
        mixerUsesServos = true;
    }

    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++)
        servoComputeScalingFactors(i);

    if (feature(FEATURE_TRIFLIGHT) && (mixerConfig()->platformType == PLATFORM_TRICOPTER))
        triMixerInit(servoParamsMutable(SERVO_TRICOPTER_TAIL), &servo[SERVO_TRICOPTER_TAIL]);
}

int getServoCount(void)
{
    return (servoRuleCount ? (1 + maxServoIndex - minServoIndex) : 0);
}

void loadCustomServoMixer(void)
{
    // Purge current servo mixer
    servoRuleCount = 0;
    minServoIndex = 255;
    maxServoIndex = 0;
    memset(currentServoMixer, 0, sizeof(currentServoMixer));

    // Load custom mixer into currentServoMixer
    for (int i = 0; i < MAX_SERVO_RULES; i++) {

        if (customServoMixers(i)->rate == 0)  // Finished loading all rules
            break;

        if (customServoMixers(i)->targetChannel < minServoIndex)
            minServoIndex = customServoMixers(i)->targetChannel;

        if (customServoMixers(i)->targetChannel > maxServoIndex)
            maxServoIndex = customServoMixers(i)->targetChannel;

        memcpy(&currentServoMixer[i], customServoMixers(i), sizeof(servoMixer_t));
        servoRuleCount++;
    }
}

static void filterServos(void)
{
    if (servoConfig()->servo_lowpass_freq) {

        // Initialize servo lowpass filters
        // NOTE: Servos are calculated at gyro looptime rate
        // TODO: Move this into an init function?
        if (!servoFilterIsSet) {
            for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
                biquadFilterInitLPF(&servoFilter[i], servoConfig()->servo_lowpass_freq, getLooptime());
                biquadFilterReset(&servoFilter[i], servo[i]);
            }
            servoFilterIsSet = true;
        }

        // Apply filters
        for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++)
            servo[i] = (int16_t)lrintf(biquadFilterApply(&servoFilter[i], (float)servo[i]));
    }

    // Constrain servo positions to prevent physical damage
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++)
        servo[i] = constrain(servo[i], servoParams(i)->min, servoParams(i)->max);
}

void writeServos(void)
{
    filterServos();
    
#if !defined(SITL_BUILD)
    int servoIndex = 0;
    bool disableTricopterServo = false;

    // Center tail servo in the case of a disarmed tricopter with tri_unarmed_servo disabled
    if (mixerConfig()->platformType == PLATFORM_TRICOPTER && !ARMING_FLAG(ARMED) &&
    !servoConfig()->tri_unarmed_servo && !FLIGHT_MODE(TAILTUNE_MODE)) {
        disableTricopterServo = true;
    }

    for (int i = minServoIndex; i <= maxServoIndex; i++) {
        if (disableTricopterServo && (i == SERVO_TRICOPTER_TAIL))
            pwmWriteServo(servoIndex++, servoParams(i)->middle);
        else
            pwmWriteServo(servoIndex++, servo[i]);
    }
#endif
}

void servoMixer(float dT)
{
    int16_t input[INPUT_SOURCE_COUNT];  // Range [-500, 500]

    if (FLIGHT_MODE(MANUAL_MODE)) {
        input[INPUT_STABILIZED_ROLL] = rcCommand[ROLL];
        input[INPUT_STABILIZED_PITCH] = rcCommand[PITCH];
        input[INPUT_STABILIZED_YAW] = rcCommand[YAW];
    } else {  // Assisted modes (gyro only or gyro+acc according to AUX configuration in GUI)
        input[INPUT_STABILIZED_ROLL] = axisPID[ROLL];
        input[INPUT_STABILIZED_PITCH] = axisPID[PITCH];
        input[INPUT_STABILIZED_YAW] = axisPID[YAW];

        // Reverse yaw when inverted in 3D mode (only for multirotor and tricopter)
        if (feature(FEATURE_REVERSIBLE_MOTORS) && (rxGetChannelValue(THROTTLE) < PWM_RANGE_MIDDLE) &&
        (mixerConfig()->platformType == PLATFORM_MULTIROTOR || mixerConfig()->platformType == PLATFORM_TRICOPTER)) {
            input[INPUT_STABILIZED_YAW] *= -1;
        }
    }

    input[INPUT_STABILIZED_ROLL_PLUS] = constrain(input[INPUT_STABILIZED_ROLL], 0, 1000);
    input[INPUT_STABILIZED_ROLL_MINUS] = constrain(input[INPUT_STABILIZED_ROLL], -1000, 0);
    input[INPUT_STABILIZED_PITCH_PLUS] = constrain(input[INPUT_STABILIZED_PITCH], 0, 1000);
    input[INPUT_STABILIZED_PITCH_MINUS] = constrain(input[INPUT_STABILIZED_PITCH], -1000, 0);
    input[INPUT_STABILIZED_YAW_PLUS] = constrain(input[INPUT_STABILIZED_YAW], 0, 1000);
    input[INPUT_STABILIZED_YAW_MINUS] = constrain(input[INPUT_STABILIZED_YAW], -1000, 0);

    input[INPUT_FEATURE_FLAPS] = FLIGHT_MODE(FLAPERON) ? servoConfig()->flaperon_throw_offset : 0;

    input[INPUT_MAX] = 500;
#ifdef USE_PROGRAMMING_FRAMEWORK
    input[INPUT_GVAR_0] = constrain(gvGet(0), -1000, 1000);
    input[INPUT_GVAR_1] = constrain(gvGet(1), -1000, 1000);
    input[INPUT_GVAR_2] = constrain(gvGet(2), -1000, 1000);
    input[INPUT_GVAR_3] = constrain(gvGet(3), -1000, 1000);
    input[INPUT_GVAR_4] = constrain(gvGet(4), -1000, 1000);
    input[INPUT_GVAR_5] = constrain(gvGet(5), -1000, 1000);
    input[INPUT_GVAR_6] = constrain(gvGet(6), -1000, 1000);
    input[INPUT_GVAR_7] = constrain(gvGet(7), -1000, 1000);
#endif

    if (IS_RC_MODE_ACTIVE(BOXCAMSTAB)) {
        input[INPUT_GIMBAL_PITCH] = scaleRange(attitude.values.pitch, -900, 900, -500, +500);
        input[INPUT_GIMBAL_ROLL] = scaleRange(attitude.values.roll, -1800, 1800, -500, +500);
    } else {
        input[INPUT_GIMBAL_PITCH] = 0;
        input[INPUT_GIMBAL_ROLL] = 0;
    }

    input[INPUT_STABILIZED_THROTTLE] = mixerThrottleCommand - 1000 - 500;  // Since it derives from rcCommand or mincommand and must be [-500:+500]

    // Center the RC input value
    // [1000, 2000] -> [-500, 500]
#define GET_RX_CHANNEL_INPUT(x) (rxGetChannelValue(x) - PWM_RANGE_MIDDLE)
    input[INPUT_RC_ROLL]     = GET_RX_CHANNEL_INPUT(ROLL);
    input[INPUT_RC_PITCH]    = GET_RX_CHANNEL_INPUT(PITCH);
    input[INPUT_RC_YAW]      = GET_RX_CHANNEL_INPUT(YAW);
    input[INPUT_RC_THROTTLE] = GET_RX_CHANNEL_INPUT(THROTTLE);
    input[INPUT_RC_CH5]      = GET_RX_CHANNEL_INPUT(AUX1);
    input[INPUT_RC_CH6]      = GET_RX_CHANNEL_INPUT(AUX2);
    input[INPUT_RC_CH7]      = GET_RX_CHANNEL_INPUT(AUX3);
    input[INPUT_RC_CH8]      = GET_RX_CHANNEL_INPUT(AUX4);
    input[INPUT_RC_CH9]      = GET_RX_CHANNEL_INPUT(AUX5);
    input[INPUT_RC_CH10]     = GET_RX_CHANNEL_INPUT(AUX6);
    input[INPUT_RC_CH11]     = GET_RX_CHANNEL_INPUT(AUX7);
    input[INPUT_RC_CH12]     = GET_RX_CHANNEL_INPUT(AUX8);
    input[INPUT_RC_CH13]     = GET_RX_CHANNEL_INPUT(AUX9);
    input[INPUT_RC_CH14]     = GET_RX_CHANNEL_INPUT(AUX10);
    input[INPUT_RC_CH15]     = GET_RX_CHANNEL_INPUT(AUX11);
    input[INPUT_RC_CH16]     = GET_RX_CHANNEL_INPUT(AUX12);
#undef GET_RX_CHANNEL_INPUT

    // This bypasses triflight, but that's probably fine since software support is unlikely to ever happen
#ifdef USE_SIMULATOR
    simulatorData.input[INPUT_STABILIZED_ROLL] = input[INPUT_STABILIZED_ROLL];
    simulatorData.input[INPUT_STABILIZED_PITCH] = input[INPUT_STABILIZED_PITCH];
    simulatorData.input[INPUT_STABILIZED_YAW] = input[INPUT_STABILIZED_YAW];
    simulatorData.input[INPUT_STABILIZED_THROTTLE] = input[INPUT_STABILIZED_THROTTLE];
#endif

    // Zero all servos
    // TODO: Can this be removed? servosInit() has already set default values for the full array
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++)
        servo[i] = 0;

    // Mix servos according to rules
    for (int i = 0; i < servoRuleCount; i++) {

#ifdef USE_PROGRAMMING_FRAMEWORK
        // Skip rule if its condition is not true
        if (!logicConditionGetValue(currentServoMixer[i].conditionId))
            continue;
#endif
        const uint8_t target = currentServoMixer[i].targetChannel;
        const uint8_t from = currentServoMixer[i].inputSource;

        // Limit servo speed
        // 0 = no limiting
        // 1 = 10us/s -> full servo sweep (from 1000 to 2000) takes 100s
        // 10 = 100us/s -> full sweep takes 10s
        int16_t inputLimited = (int16_t) rateLimitFilterApply4(&servoSpeedLimitFilter[i], input[from], currentServoMixer[i].speed * 10, dT);
        servo[target] += ((int32_t)inputLimited * currentServoMixer[i].rate) / 100;
    }

    // Set all throttle-controlled servos to lowest position if not armed
    if (!ARMING_FLAG(ARMED)) {
        for (int i = 0; i < servoRuleCount; i++) {
            const uint8_t target = currentServoMixer[i].targetChannel;
            const uint8_t from = currentServoMixer[i].inputSource;

            if (from == INPUT_STABILIZED_THROTTLE || from == INPUT_RC_THROTTLE)
                servo[target] = motorConfig()->mincommand;
        }
    }

    // Convert from raw position data to pulse width
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++) {

        // Apply servo's rate setting
        servo[i] = ((int32_t)servoParams(i)->rate * servo[i]) / 100L;

        // Perform output scaling to match servo min/max values
        // NOTE: Result not always in bounds
        servo[i] = (int16_t)(servo[i] * ((servo[i] > 0) ? servoMetadata[i].scaleMax : servoMetadata[i].scaleMin));

        // Add midpoint offset
        servo[i] += servoParams(i)->middle;

        // Constrain position to prevent physical damage
        servo[i] = constrain(servo[i], servoParams(i)->min, servoParams(i)->max);
    }

    // If triflight is active, recalculate the tail servo
    if (feature(FEATURE_TRIFLIGHT) && (mixerConfig()->platformType == PLATFORM_TRICOPTER))
        triServoMixer((float)axisPID[YAW], dT);
}

#define SERVO_AUTOTRIM_TIMER_MS     2000

typedef enum {
    AUTOTRIM_IDLE,
    AUTOTRIM_COLLECTING,
    AUTOTRIM_SAVE_PENDING,
    AUTOTRIM_DONE,
} servoAutotrimState_e;

void processServoAutotrimMode(void)
{
    static servoAutotrimState_e trimState = AUTOTRIM_IDLE;
    static timeMs_t trimStartedAt;

    static int16_t servoMiddleBackup[MAX_SUPPORTED_SERVOS];
    static int32_t servoMiddleAccum[MAX_SUPPORTED_SERVOS];
    static int32_t servoMiddleAccumCount[MAX_SUPPORTED_SERVOS];

    if (IS_RC_MODE_ACTIVE(BOXAUTOTRIM)) {
        switch (trimState) {
            case AUTOTRIM_IDLE:
                if (ARMING_FLAG(ARMED)) {
                    for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
                        for (int i = 0; i < servoRuleCount; i++) {
                            const uint8_t target = currentServoMixer[i].targetChannel;
                            const uint8_t source = currentServoMixer[i].inputSource;
                            if (source == axis) {
                                servoMiddleBackup[target] = servoParams(target)->middle;
                                servoMiddleAccum[target] = 0;
                                servoMiddleAccumCount[target] = 0;
                            }
                        }
                    }
                    trimStartedAt = millis();
                    trimState = AUTOTRIM_COLLECTING;
                }
                else
                    break;
                // Fallthru

            case AUTOTRIM_COLLECTING:
                if (ARMING_FLAG(ARMED)) {
                    for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
                        for (int i = 0; i < servoRuleCount; i++) {
                            const uint8_t target = currentServoMixer[i].targetChannel;
                            const uint8_t source = currentServoMixer[i].inputSource;
                            if (source == axis) {
                                servoMiddleAccum[target] += servo[target];
                                servoMiddleAccumCount[target]++;
                            }
                        }
                    }

                    if ((millis() - trimStartedAt) > SERVO_AUTOTRIM_TIMER_MS) {
                        for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
                            for (int i = 0; i < servoRuleCount; i++) {
                                const uint8_t target = currentServoMixer[i].targetChannel;
                                const uint8_t source = currentServoMixer[i].inputSource;
                                if (source == axis)
                                    servoParamsMutable(target)->middle = servoMiddleAccum[target] / servoMiddleAccumCount[target];
                            }
                        }
                        trimState = AUTOTRIM_SAVE_PENDING;
                        pidResetErrorAccumulators(); //Reset Iterm since new midpoints override previously acumulated errors
                    }
                }
                else
                    trimState = AUTOTRIM_IDLE;

                break;

            case AUTOTRIM_SAVE_PENDING:
                // Wait for disarm and save to EEPROM
                if (!ARMING_FLAG(ARMED)) {
                    saveConfigAndNotify();
                    trimState = AUTOTRIM_DONE;
                }
                break;

            case AUTOTRIM_DONE:
                break;
        }
    }
    else {
        // We are deactivating servo trim - restore servo midpoints
        if (trimState == AUTOTRIM_SAVE_PENDING) {
            for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
                for (int i = 0; i < servoRuleCount; i++) {
                    const uint8_t target = currentServoMixer[i].targetChannel;
                    const uint8_t source = currentServoMixer[i].inputSource;
                    if (source == axis)
                        servoParamsMutable(target)->middle = servoMiddleBackup[target];
                }
            }
        }

        trimState = AUTOTRIM_IDLE;
    }
}

#define SERVO_AUTOTRIM_FILTER_CUTOFF    1       // LPF cutoff frequency
#define SERVO_AUTOTRIM_CENTER_MIN       1300
#define SERVO_AUTOTRIM_CENTER_MAX       1700
#define SERVO_AUTOTRIM_UPDATE_SIZE      5
#define SERVO_AUTOTRIM_ATTITUDE_LIMIT   50       // 5 degrees

void processContinuousServoAutotrim(const float dT)
{
    static timeMs_t lastUpdateTimeMs;
    static servoAutotrimState_e trimState = AUTOTRIM_IDLE;    
    static uint32_t servoMiddleUpdateCount;

    const float rotRateMagnitudeFiltered = pt1FilterApply4(&rotRateFilter, fast_fsqrtf(vectorNormSquared(&imuMeasuredRotationBF)), SERVO_AUTOTRIM_FILTER_CUTOFF, dT);
    const float targetRateMagnitudeFiltered = pt1FilterApply4(&targetRateFilter, getTotalRateTarget(), SERVO_AUTOTRIM_FILTER_CUTOFF, dT);

    if (ARMING_FLAG(ARMED)) {
        trimState = AUTOTRIM_COLLECTING;
        if ((millis() - lastUpdateTimeMs) > 500) {
            const bool planeIsFlyingStraight = rotRateMagnitudeFiltered <= DEGREES_TO_RADIANS(servoConfig()->servo_autotrim_rotation_limit);
            const bool noRotationCommanded = targetRateMagnitudeFiltered <= servoConfig()->servo_autotrim_rotation_limit;
            const bool sticksAreCentered = !areSticksDeflected();
            const bool planeIsFlyingLevel = ABS(attitude.values.pitch + DEGREES_TO_DECIDEGREES(getFixedWingLevelTrim())) <= SERVO_AUTOTRIM_ATTITUDE_LIMIT 
                                            && ABS(attitude.values.roll) <= SERVO_AUTOTRIM_ATTITUDE_LIMIT;
            if (
                planeIsFlyingStraight && 
                noRotationCommanded && 
                planeIsFlyingLevel &&
                sticksAreCentered &&
                !FLIGHT_MODE(MANUAL_MODE) && 
                isGPSHeadingValid() // TODO: proper flying detection
            ) { 
                // Plane is flying straight and level: trim servos
                for (int axis = FD_ROLL; axis <= FD_PITCH; axis++) {
                    // For each stabilized axis, add 5 units of I-term to all associated servo midpoints
                    const float axisIterm = getAxisIterm(axis);
                    if (fabsf(axisIterm) > SERVO_AUTOTRIM_UPDATE_SIZE) {
                        const int8_t ItermUpdate = axisIterm > 0.0f ? SERVO_AUTOTRIM_UPDATE_SIZE : -SERVO_AUTOTRIM_UPDATE_SIZE;
                        for (int i = 0; i < servoRuleCount; i++) {
#ifdef USE_PROGRAMMING_FRAMEWORK
                            if (!logicConditionGetValue(currentServoMixer[i].conditionId))
                                continue;
#endif
                            const uint8_t target = currentServoMixer[i].targetChannel;
                            const uint8_t source = currentServoMixer[i].inputSource;
                            if (source == axis) {
                                // Convert axis I-term to servo PWM and add to midpoint
                                const float mixerRate = currentServoMixer[i].rate / 100.0f;
                                const float servoRate = servoParams(target)->rate / 100.0f;
                                servoParamsMutable(target)->middle += (int16_t)(ItermUpdate * mixerRate * servoRate);
                                servoParamsMutable(target)->middle = constrain(servoParamsMutable(target)->middle, SERVO_AUTOTRIM_CENTER_MIN, SERVO_AUTOTRIM_CENTER_MAX);
                            }
                        }
                        pidReduceErrorAccumulators(ItermUpdate, axis);
                    }
                }
                servoMiddleUpdateCount++;
            }
            // Reset timer
            lastUpdateTimeMs = millis();
        }
    } else if (trimState == AUTOTRIM_COLLECTING) {
        // We have disarmed, save midpoints to EEPROM
        saveConfigAndNotify();
        trimState = AUTOTRIM_IDLE;
    }

    // Debug
    DEBUG_SET(DEBUG_AUTOTRIM, 0, servoParams(2)->middle);
    DEBUG_SET(DEBUG_AUTOTRIM, 2, servoParams(3)->middle);
    DEBUG_SET(DEBUG_AUTOTRIM, 4, servoParams(4)->middle);
    DEBUG_SET(DEBUG_AUTOTRIM, 6, servoParams(5)->middle);
    DEBUG_SET(DEBUG_AUTOTRIM, 1, servoMiddleUpdateCount);
    DEBUG_SET(DEBUG_AUTOTRIM, 3, MAX(RADIANS_TO_DEGREES(rotRateMagnitudeFiltered), targetRateMagnitudeFiltered));
    DEBUG_SET(DEBUG_AUTOTRIM, 5, axisPID_I[FD_ROLL]);
    DEBUG_SET(DEBUG_AUTOTRIM, 7, axisPID_I[FD_PITCH]);    
}

void processServoAutotrim(const float dT) {
#ifdef USE_SIMULATOR
    if (ARMING_FLAG(SIMULATOR_MODE_HITL))
        return;
#endif
    if (feature(FEATURE_FW_AUTOTRIM))
        processContinuousServoAutotrim(dT);
    else
        processServoAutotrimMode();
}

bool isServoOutputEnabled(void)
{
    return servoOutputEnabled;
}

void setServoOutputEnabled(bool flag)
{
    servoOutputEnabled = flag;
}

bool isMixerUsingServos(void)
{
    return mixerUsesServos;
}
