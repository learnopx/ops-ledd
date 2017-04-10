/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 */

/************************************************************************//**
 * @defgroup ops-ledd LED Daemon
 * This module is the platform daemon that processess and manages LEDs
 * for all subsystems in the switch that have LEDs.
 * @{
 *
 * @file
 * Header for platform LED daemon
 *
 * @defgroup ledd_public Public Interface
 * Public API for the platform LED daemon
 *
 * The platform LED daemon is responsible for managing and reporting status
 * for LEDs in any subsystem that has LEDs that can be managed or reported.
 *
 * @{
 *
 * Public APIs
 *
 * Command line options:
 *
 *     usage: ops-ledd [OPTIONS] [DATABASE]
 *     where DATABASE is a socket on which ovsdb-server is listening
 *           (default: "unix:/var/run/openvswitch/db.sock").
 *
 *     Active DATABASE connection methods:
 *          tcp:IP:PORT             PORT at remote IP
 *          ssl:IP:PORT             SSL PORT at remote IP
 *          unix:FILE               Unix domain socket named FILE
 *     PKI configuration (required to use SSL):
 *          -p, --private-key=FILE  file with private key
 *          -c, --certificate=FILE  file with certificate for private key
 *          -C, --ca-cert=FILE      file with peer CA certificate
 *          --bootstrap-ca-cert=FILE  file with peer CA certificate to read or create
 *
 *     Daemon options:
 *          --detach                run in background as daemon
 *          --no-chdir              do not chdir to '/'
 *          --pidfile[=FILE]        create pidfile (default: /var/run/openvswitch/ops-ledd.pid)
 *          --overwrite-pidfile     with --pidfile, start even if already running
 *
 *     Logging options:
 *          -vSPEC, --verbose=SPEC   set logging levels
 *          -v, --verbose            set maximum verbosity level
 *          --log-file[=FILE]        enable logging to specified FILE
 *                                  (default: /var/log/openvswitch/ops-ledd.log)
 *          --syslog-target=HOST:PORT  also send syslog msgs to HOST:PORT via UDP
 *
 *     Other options:
 *          --unixctl=SOCKET        override default control socket name
 *          -h, --help              display this help message
 *          -V, --version           display version information
 *
 *
 * ovs-apptcl options:
 *
 *      Support dump: ovs-appctl -t ops-ledd ops-ledd/dump
 *
 *
 * OVSDB elements usage
 *
 *     Creation: The following rows/cols are created by ops-ledd
 *               rows in led table
 *               led:id
 *               led:state
 *               led:status
 *
 *     Written: The following cols are written by ops-ledd
 *              led:status
 *              daemon["ops-ledd"]:cur_hw
 *              subsystem:leds
 *
 *     Read: The following cols are read by ops-ledd
 *           led:state
 *           subsystem:name
 *           subsystem:hw_desc_dir
 *
 * Linux Files:
 *
 *     The following files are written by ops-ledd
 *           /var/run/openvswitch/ops-ledd.pid: Process ID for the ops-ledd daemon
 *           /var/run/openvswitch/ops-ledd.<pid>.ctl: unixctl socket for the ops-ledd daemon
 *
 * @}
 ***************************************************************************/


#ifndef _LEDD_H_
#define _LEDD_H_

#include <stdbool.h>
#include "shash.h"
#include "config-yaml.h"

/* **************** DEFINES ************* */

#define NAME_IN_DAEMON_TABLE "ops-ledd" /*!< Name identifier for this daemon in the OVSDB daemon table */

#define LEDD_LED_TYPE_LOC       "loc" /*!< Name identifier for LED type loc */

VLOG_DEFINE_THIS_MODULE(ops_ledd);
COVERAGE_DEFINE(ledd_reconfigure);

/* **************** TYPEDEFS  ************* */

/************************************************************************//**
 * char array containing the string name for supported led types. These
 * are defined in this header file.
 ***************************************************************************/
const char *led_type_strings[] = {
    LEDD_LED_TYPE_LOC     /*!< LED type "loc" */
};

/************************************************************************//**
 * char array containing the string name for supported led states. These
 * are defined in the OVS schema for the LED table.
 ***************************************************************************/
const char *led_state_strings[] = {
    OVSREC_LED_STATE_FLASHING,          /*!< LED state "flashing" */
    OVSREC_LED_STATE_OFF,               /*!< LED state "off" */
    OVSREC_LED_STATE_ON                 /*!< LED state "on" */
};

/************************************************************************//**
 * char array containing the string name for supported led statuses. These
 * are defined in the OVS schema for the LED table.
 ***************************************************************************/
const char *led_status_strings[] = {
    OVSREC_LED_STATUS_FAULT,            /*!< LED status "fault" */
    OVSREC_LED_STATUS_OK,               /*!< LED status "ok" */
    OVSREC_LED_STATUS_UNINITIALIZED     /*!< LED status "uninitialized" */
};

/************************************************************************//**
 * ENUM to indicate if the subsystem is valid (OK), or not (IGNORE).
 ***************************************************************************/
enum subsysstatus {
    LEDD_SUBSYS_STATUS_OK,              /*!< Subsystem is ok, process */
    LEDD_SUBSYS_STATUS_IGNORE           /*!< Subsystem not ok, don't process */
};

/************************************************************************//**
 * STRUCT used to keep information about each subsystem in the OVSDB,
 * including what LED information is applicable.
 ***************************************************************************/
struct locl_subsystem {
    char *name;                         /*!< Name of the subsystem */
    bool marked;                        /*!< True if subsystem exists*/
    struct locl_subsystem *parent_subsystem; /*!< parent subsystem */
    int num_leds;                       /*!< Number of LEDs in subsystem */
    int num_types;                      /*!< Number of LED types in subsystem */
    struct shash subsystem_leds;        /*!< shash of locl_led structs*/
    struct shash subsystem_types;       /*!< shash of YamlLedType structs */
    enum subsysstatus subsys_status;    /*!< status {OK, IGNORE} */
};

/************************************************************************//**
 * STRUCT used to keep information about each LED in the subsystem.
 ***************************************************************************/
struct locl_led {
    char *name;                         /*!< LED name */
    struct locl_subsystem *subsystem;   /*!< Subsystem this LED is in */
    const YamlLed *yaml_led;            /*!< YamlLed struct for this LED */
    YamlLedTypeSettings *settings;      /*!< Settings for this LED */
    enum ovsrec_led_state_e state;      /*!< Last state in OVSDB */
    enum ovsrec_led_status_e status;    /*!< Last status in OVSDB */
};

#endif /* _LEDD_H_ */
/** @} end of group ops-ledd */
