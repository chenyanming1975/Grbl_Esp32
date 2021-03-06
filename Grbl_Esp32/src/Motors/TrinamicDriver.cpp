/*
    TrinamicDriver.cpp
    This is used for Trinamic SPI controlled stepper motor drivers.

    Part of Grbl_ESP32
    2020 -	Bart Dring

    Grbl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    Grbl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/
#include "TrinamicDriver.h"

#include <TMCStepper.h>

#ifdef USE_I2S_OUT

// Override default function and insert a short delay
void TMC2130Stepper::switchCSpin(bool state) {
    digitalWrite(_pinCS, state);
    i2s_out_delay();
}
#endif

namespace Motors {
    uint8_t TrinamicDriver::get_next_index() {
#ifdef TRINAMIC_DAISY_CHAIN
        static uint8_t index = 1;  // they start at 1
        return index++;
#else
        return -1;
#endif
    }
    TrinamicDriver* TrinamicDriver::List = NULL;

    TrinamicDriver::TrinamicDriver(uint8_t  axis_index,
                                   uint8_t  step_pin,
                                   uint8_t  dir_pin,
                                   uint8_t  disable_pin,
                                   uint8_t  cs_pin,
                                   uint16_t driver_part_number,
                                   float    r_sense,
                                   int8_t   spi_index) :
        StandardStepper(axis_index, step_pin, dir_pin, disable_pin),
        _homing_mode(TRINAMIC_HOMING_MODE), _cs_pin(cs_pin), _driver_part_number(driver_part_number), _r_sense(r_sense),
        _spi_index(spi_index) {
        _has_errors = false;
        if (_driver_part_number == 2130) {
            tmcstepper = new TMC2130Stepper(_cs_pin, _r_sense, _spi_index);
        } else if (_driver_part_number == 5160) {
            tmcstepper = new TMC5160Stepper(_cs_pin, _r_sense, _spi_index);
        } else {
            grbl_msg_sendf(CLIENT_SERIAL,
                           MsgLevel::Info,
                           "%s Unsupported Trinamic part number TMC%d",
                           reportAxisNameMsg(_axis_index, _dual_axis_index),
                           _driver_part_number);
            _has_errors = true;  // This motor cannot be used
            return;
        }

        _has_errors = false;
        init_step_dir_pins();  // from StandardStepper

        digitalWrite(_cs_pin, HIGH);
        pinMode(_cs_pin, OUTPUT);

        // use slower speed if I2S
        if (_cs_pin >= I2S_OUT_PIN_BASE) {
            tmcstepper->setSPISpeed(TRINAMIC_SPI_FREQ);
        }

        link = List;
        List = this;

        // init() must be called later, after all TMC drivers have CS pins setup.
    }

    void TrinamicDriver::init() {
        if (_has_errors) {
            return;
        }

        // Display the stepper library version message once, before the first
        // TMC config message.  Link is NULL for the first TMC instance.
        if (!link) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "TMCStepper Library Ver. 0x%06x", TMCSTEPPER_VERSION);
        }

        config_message();

        SPI.begin();  // this will get called for each motor, but does not seem to hurt anything

        tmcstepper->begin();

        _has_errors = !test();  // Try communicating with motor. Prints an error if there is a problem.

        read_settings();  // pull info from settings
        set_mode(false);

        // After initializing all of the TMC drivers, create a task to
        // display StallGuard data.  List == this for the final instance.
        if (List == this) {
            xTaskCreatePinnedToCore(readSgTask,    // task
                                    "readSgTask",  // name for task
                                    4096,          // size of task stack
                                    NULL,          // parameters
                                    1,             // priority
                                    NULL,
                                    0  // core
            );
        }
    }

    /*
    This is the startup message showing the basic definition
*/
    void TrinamicDriver::config_message() {
        grbl_msg_sendf(CLIENT_SERIAL,
                       MsgLevel::Info,
                       "%s Trinamic TMC%d Step:%s Dir:%s CS:%s Disable:%s Index:%d %s",
                       reportAxisNameMsg(_axis_index, _dual_axis_index),
                       _driver_part_number,
                       pinName(_step_pin).c_str(),
                       pinName(_dir_pin).c_str(),
                       pinName(_cs_pin).c_str(),
                       pinName(_disable_pin).c_str(),
                       _spi_index,
                       reportAxisLimitsMsg(_axis_index));
    }

    bool TrinamicDriver::test() {
        if (_has_errors) {
            return false;
        }
        switch (tmcstepper->test_connection()) {
            case 1:
                grbl_msg_sendf(CLIENT_SERIAL,
                               MsgLevel::Info,
                               "%s Trinamic driver test failed. Check connection",
                               reportAxisNameMsg(_axis_index, _dual_axis_index));
                return false;
            case 2:
                grbl_msg_sendf(CLIENT_SERIAL,
                               MsgLevel::Info,
                               "%s Trinamic driver test failed. Check motor power",
                               reportAxisNameMsg(_axis_index, _dual_axis_index));
                return false;
            default:
                // driver responded, so check for other errors from the DRV_STATUS register

                TMC2130_n ::DRV_STATUS_t status { 0 };  // a useful struct to access the bits.
                status.sr = tmcstepper->DRV_STATUS();

                bool err = false;
                // look for open or short to ground on a and b
                if (status.s2ga || status.s2gb) {
                    grbl_msg_sendf(CLIENT_SERIAL,
                                   MsgLevel::Info,
                                   "%s Motor Short Coil a:%s b:%s",
                                   reportAxisNameMsg(_axis_index, _dual_axis_index),
                                   status.s2ga ? "Y" : "N",
                                   status.s2gb ? "Y" : "N");
                    err = true;
                }
                // check for over temp or pre-warning
                if (status.ot || status.otpw) {
                    grbl_msg_sendf(CLIENT_SERIAL,
                                   MsgLevel::Info,
                                   "%s Driver Temp Warning:%s Fault:%s",
                                   reportAxisNameMsg(_axis_index, _dual_axis_index),
                                   status.otpw ? "Y" : "N",
                                   status.ot ? "Y" : "N");
                    err = true;
                }

                if (err) {
                    return false;
                }

                grbl_msg_sendf(
                    CLIENT_SERIAL, MsgLevel::Info, "%s Trinamic driver test passed", reportAxisNameMsg(_axis_index, _dual_axis_index));
                return true;
        }
    }

    /*
    Read setting and send them to the driver. Called at init() and whenever related settings change
    both are stored as float Amps, but TMCStepper library expects...
    uint16_t run (mA)
    float hold (as a percentage of run)
*/

    void TrinamicDriver::read_settings() {
        if (_has_errors) {
            return;
        }

        uint16_t run_i_ma = (uint16_t)(axis_settings[_axis_index]->run_current->get() * 1000.0);
        float    hold_i_percent;

        if (axis_settings[_axis_index]->run_current->get() == 0)
            hold_i_percent = 0;
        else {
            hold_i_percent = axis_settings[_axis_index]->hold_current->get() / axis_settings[_axis_index]->run_current->get();
            if (hold_i_percent > 1.0)
                hold_i_percent = 1.0;
        }
        //grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "%s Current run %d hold %f", reportAxisNameMsg(_axis_index, _dual_axis_index), run_i_ma, hold_i_percent);

        tmcstepper->microsteps(axis_settings[_axis_index]->microsteps->get());
        tmcstepper->rms_current(run_i_ma, hold_i_percent);
    }

    bool TrinamicDriver::set_homing_mode(bool isHoming) {
        set_mode(isHoming);
        return true;
    }

    /*
    There are ton of settings. I'll start by grouping then into modes for now.
    Many people will want quiet and stallgaurd homing. Stallguard only run in
    Coolstep mode, so it will need to switch to Coolstep when homing
*/
    void TrinamicDriver::set_mode(bool isHoming) {
        if (_has_errors) {
            return;
        }

        TrinamicMode newMode = isHoming ? TRINAMIC_HOMING_MODE : TRINAMIC_RUN_MODE;

        if (newMode == _mode) {
            return;
        }
        _mode = newMode;

        switch (_mode) {
            case TrinamicMode ::StealthChop:
                //grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "StealthChop");
                tmcstepper->en_pwm_mode(true);
                tmcstepper->pwm_autoscale(true);
                tmcstepper->diag1_stall(false);
                break;
            case TrinamicMode ::CoolStep:
                //grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Coolstep");
                tmcstepper->en_pwm_mode(false);
                tmcstepper->pwm_autoscale(false);
                tmcstepper->TCOOLTHRS(NORMAL_TCOOLTHRS);  // when to turn on coolstep
                tmcstepper->THIGH(NORMAL_THIGH);
                break;
            case TrinamicMode ::StallGuard:
                //grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Stallguard");
                tmcstepper->en_pwm_mode(false);
                tmcstepper->pwm_autoscale(false);
                tmcstepper->TCOOLTHRS(calc_tstep(homing_feed_rate->get(), 150.0));
                tmcstepper->THIGH(calc_tstep(homing_feed_rate->get(), 60.0));
                tmcstepper->sfilt(1);
                tmcstepper->diag1_stall(true);  // stallguard i/o is on diag1
                tmcstepper->sgt(axis_settings[_axis_index]->stallguard->get());
                break;
            default:
                grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "TRINAMIC_MODE_UNDEFINED");
        }
    }

    /*
    This is the stallguard tuning info. It is call debug, so it could be generic across all classes.
*/
    void TrinamicDriver::debug_message() {
        if (_has_errors) {
            return;
        }
        uint32_t tstep = tmcstepper->TSTEP();

        if (tstep == 0xFFFFF || tstep < 1) {  // if axis is not moving return
            return;
        }
        float feedrate = st_get_realtime_rate();  //* settings.microsteps[axis_index] / 60.0 ; // convert mm/min to Hz

        grbl_msg_sendf(CLIENT_SERIAL,
                       MsgLevel::Info,
                       "%s Stallguard %d   SG_Val: %04d   Rate: %05.0f mm/min SG_Setting:%d",
                       reportAxisNameMsg(_axis_index, _dual_axis_index),
                       tmcstepper->stallguard(),
                       tmcstepper->sg_result(),
                       feedrate,
                       axis_settings[_axis_index]->stallguard->get());
    }

    // calculate a tstep from a rate
    // tstep = TRINAMIC_FCLK / (time between 1/256 steps)
    // This is used to set the stallguard window from the homing speed.
    // The percent is the offset on the window
    uint32_t TrinamicDriver::calc_tstep(float speed, float percent) {
        float tstep =
            speed / 60.0 * axis_settings[_axis_index]->steps_per_mm->get() * (float)(256 / axis_settings[_axis_index]->microsteps->get());
        tstep = TRINAMIC_FCLK / tstep * percent / 100.0;

        return static_cast<uint32_t>(tstep);
    }

    // this can use the enable feature over SPI. The dedicated pin must be in the enable mode,
    // but that can be hardwired that way.
    void TrinamicDriver::set_disable(bool disable) {
        if (_has_errors) {
            return;
        }

        if (_disabled == disable) {
            return;
        }

        _disabled = disable;

        digitalWrite(_disable_pin, _disabled);

#ifdef USE_TRINAMIC_ENABLE
        if (_disabled) {
            tmcstepper->toff(TRINAMIC_TOFF_DISABLE);
        } else {
            if (_mode == TrinamicMode::StealthChop) {
                tmcstepper->toff(TRINAMIC_TOFF_STEALTHCHOP);
            } else {
                tmcstepper->toff(TRINAMIC_TOFF_COOLSTEP);
            }
        }
#endif
        // the pin based enable could be added here.
        // This would be for individual motors, not the single pin for all motors.
    }

    // Prints StallGuard data that is useful for tuning.
    void TrinamicDriver::readSgTask(void* pvParameters) {
        TickType_t       xLastWakeTime;
        const TickType_t xreadSg = 200;  // in ticks (typically ms)
        auto             n_axis  = number_axis->get();

        xLastWakeTime = xTaskGetTickCount();  // Initialise the xLastWakeTime variable with the current time.
        while (true) {                        // don't ever return from this or the task dies
            if (motorSettingChanged) {
                motors_read_settings();
                motorSettingChanged = false;
            }
            if (stallguard_debug_mask->get() != 0) {
                if (sys.state == State::Cycle || sys.state == State::Homing || sys.state == State::Jog) {
                    for (TrinamicDriver* p = List; p; p = p->link) {
                        if (bitnum_istrue(stallguard_debug_mask->get(), p->_axis_index)) {
                            //grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "SG:%d", stallguard_debug_mask->get());
                            p->debug_message();
                        }
                    }
                }  // sys.state
            }      // if mask

            vTaskDelayUntil(&xLastWakeTime, xreadSg);

            static UBaseType_t uxHighWaterMark = 0;
            reportTaskStackSize(uxHighWaterMark);
        }
    }
}
