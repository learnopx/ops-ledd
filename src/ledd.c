/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * All Rights Reserved.
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
 * @ingroup ops-ledd
 *
 * @file
 * Source file for the platform LED daemon
 *
 ***************************************************************************/

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <dynamic-string.h>

#include "config.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "dummy.h"
#include "fatal-signal.h"
#include "ovsdb-idl.h"
#include "poll-loop.h"
#include "simap.h"
#include "stream-ssl.h"
#include "stream.h"
#include "svec.h"
#include "timeval.h"
#include "unixctl.h"
#include "util.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "vswitch-idl.h"
#include "coverage.h"

#include "config-yaml.h"

#include "ledd.h"
#include "eventlog.h"

/* ********* GLOBALS **************** */

bool change_to_commit = false; /*!< True if need to update ovsdb */

/* global yaml config handle */
YamlConfigHandle yaml_handle;

/* define a shash (string hash) to hold the subsystems (by name) */
struct shash subsystem_data;

static struct ovsdb_idl *idl;

static unsigned int idl_seqno;

static unixctl_cb_func ledd_unixctl_dump;

static bool cur_hw_set = false; /*!< True if have updated cur_hw_set in db */

/*  ********* UTILITIES **************** */

YamlLedTypeValue
ledd_led_type_string_to_enum(char *type_string)
{
    if (strcmp(type_string, "loc") == 0) {
        return (LED_LOC);
    }

    return (LED_UNKNOWN);
} /* ledd_led_type_string_to_enum() */

YamlLedType *
ledd_get_led_type(struct locl_subsystem *subsys, char *value)
{
    struct shash_node *node;
    YamlLedType *type;

    SHASH_FOR_EACH(node, &(subsys->subsystem_types)) {
        type = (YamlLedType *)node->data;

        if (strcmp(type->type,value) == 0) {
            return (type);
        }
    }

    return ((YamlLedType *) NULL);

} /* ledd_get_led_type() */

/************************************************************************//**
 * Function that will remove the internal entry in the locl_subsystem hash
 * for any subsystem that is no longer in OVSDB.
 *
 * @todo OPS_TODO: need to remove subsystem yaml data
 * @todo OPS_TODO: verify that ovsdb has deleted the leds (automatic)
 ***************************************************************************/
static void
ledd_remove_unmarked_subsystems(void)
{
    struct shash_node *node, *next;
    struct shash_node *led_node, *led_next;
    struct shash_node *type_node, *type_next;

    /* Delete subsystems that no longer exist in the DB */

    SHASH_FOR_EACH_SAFE(node, next, &subsystem_data) {
        struct locl_subsystem *subsystem = node->data;

        if (subsystem->marked == false) {
            VLOG_DBG("removing subsystem %s", subsystem->name);

            /* delete all leds in the subsystem */
            SHASH_FOR_EACH_SAFE(led_node, led_next,
                                &(subsystem->subsystem_leds)) {
                struct locl_led *led = (struct locl_led *)led_node->data;

                /* delete the subsystem entry */
                shash_delete(&subsystem->subsystem_leds, led_node);

                /* free the allocated data */
                free(led->name);
                free(led);
            }

            /* delete all LED types in the subsystem */
            SHASH_FOR_EACH_SAFE(type_node, type_next,
                                &(subsystem->subsystem_types)) {

                /* delete the subsystem entry */
                shash_delete(&subsystem->subsystem_types, type_node);
            }
            free(subsystem->name);
            free(subsystem);

            shash_delete(&subsystem_data, node);

            /* OPS_TODO: need to remove subsystem yaml data */
            /* OPS_TODO: verify that ovsdb has deleted the leds (automatic) */
        }
    }
} /* ledd_remove_unmarked_subsystems() */

/************************************************************************//**
 * Function that sets the LED to the value specified in ovsdb state variable.
 *
 * Logic:
 *     - Retrieves the LED type
 *     - Retrieves the i2c settings for the LED type
 *     - Retrieves the value to write to the LED to match ovsdb state variable
 *     - Retrieves the i2c device access information
 *     - Reads the current value of the LED register
 *     - Writes the new value of the LED register (bitwise OR)
 *
 * Returns: True on success, else False for any failure
 ***************************************************************************/
bool
ledd_write_led(struct locl_subsystem *subsys, struct locl_led *led)
{
    YamlLedTypeSettings *settings;
    YamlLedType *type;
    i2c_bit_op *reg_op;
    uint32_t value;
    int rc;
    YamlLedTypeValue type_value;

    reg_op = led->yaml_led->led_access;

    /* Get the LED type */
    type = ledd_get_led_type(subsys, led->yaml_led->type);
    if (type == (YamlLedType *) NULL) {
        VLOG_DBG("ledd_write: type is null");
        return (false);
    }

    settings = &(type->settings);

    if (settings == NULL) {
        VLOG_WARN("No settings for subsystem %s, LED %s",
                subsys->name, led->name);
        return (false);
    }

    /* Get the value to set the LED to. */
    if (type->type == (char *) NULL) {
        VLOG_WARN("led type is NULL for subsystem %s, LED %s",
                subsys->name, led->name);
        return (false);
    }

    /* Get the settings for this type */
    type_value = ledd_led_type_string_to_enum(type->type);
    switch (type_value) {
        case LED_LOC:
            switch (led->state) {
                case LED_STATE_FLASHING:
                    value = settings->flashing;
                    break;
                case LED_STATE_OFF:
                    value = settings->off;
                    break;
                case LED_STATE_ON:
                    value = settings->on;
                    break;
                default:
                    VLOG_WARN("Invalid state %d for subsystem %s, LED %s",
                            led->state, subsys->name, led->name);
                    return(false);
            }
            break;
        case LED_UNKNOWN:
            /* Fall through */
        default:
            VLOG_WARN("Unknown or no type %d for subsystem %s, LED %s",
                            type_value, subsys->name, led->name);
            return(false);
    }

    rc = i2c_reg_write(yaml_handle, subsys->name, reg_op, value);

    if (rc != 0) {
        VLOG_WARN("subsystem %s: unable to set LED control register (%d)",
                    subsys->name, rc);
        return(false);
    }

    return(true);
} /* ledd_write_led() */

/* initialize the subsystem data */
static void
init_subsystems(void)
{
    shash_init(&subsystem_data);
} /* init_subsystems() */

enum ovsrec_led_status_e
ledd_status_to_enum(char *status)
{
    size_t i;

    if (status == NULL) {
        return(LED_STATUS_UNINITIALIZED);
    }

    for (i = 0; i < sizeof(led_status_strings)/sizeof(const char *); i++) {
        if (strcmp(led_status_strings[i], status) == 0) {
            return( (enum ovsrec_led_status_e)i );
        }
    }

    return(LED_STATUS_UNINITIALIZED);

} /* ledd_status_to_enum() */

enum ovsrec_led_state_e
ledd_state_to_enum(char *state)
{
    size_t i;

    if (state == NULL) {
        return(LED_STATE_OFF);
    }

    for (i = 0; i < sizeof(led_state_strings)/sizeof(const char *); i++) {
        if (strcmp(led_state_strings[i], state) == 0) {
            return( (enum ovsrec_led_state_e)i );
        }
    }

    return(LED_STATE_OFF);

} /* ledd_state_to_enum() */

static const char *
ledd_state_to_string(enum ovsrec_led_state_e state)
{
    if ((unsigned int)state <
                sizeof(led_state_strings)/sizeof(const char *)) {
        return(led_state_strings[state]);
    } else {
        return(led_state_strings[LED_STATE_OFF]);
    }
} /* ledd_state_to_string() */

static const char *
ledd_status_to_string(enum ovsrec_led_status_e status)
{
    if ((unsigned int)status < sizeof(led_status_strings)/sizeof(const char *)){
        return(led_status_strings[status]);
    } else {
        return(led_status_strings[LED_STATUS_UNINITIALIZED]);
    }
} /* ledd_status_to_string() */

static void
ledd_unixctl_dump(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct shash_node *snode;
    struct shash_node *lnode;

    ds_put_cstr(&ds, "Support Dump for Platform LED Daemon (ops-ledd)\n");

    SHASH_FOR_EACH(snode, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)snode->data;

        ds_put_format(&ds, "\nSubsystem: %s\n", subsystem->name);

        SHASH_FOR_EACH(lnode, &(subsystem->subsystem_leds)) {
            struct locl_led *led = (struct locl_led *)lnode->data;

            ds_put_format(&ds, "\tLED name: %s\n", led->name);
            ds_put_format(&ds, "\tLED type: %s\n", led->yaml_led->type);
            ds_put_format(&ds, "\tLED state: %s\n",
                                        ledd_state_to_string(led->state));
            ds_put_format(&ds, "\tLED status: %s\n",
                                        ledd_status_to_string(led->status));
        }
    }

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
} /* ledd_unixctl_dump() */

static void
usage(void)
{
    printf("%s: OpenSwitch ledd daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    stream_usage("DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  --unixctl=SOCKET        override default control socket name\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    exit(EXIT_SUCCESS);
} /* usage() */

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_UNIXCTL,
        VLOG_OPTION_ENUMS,
        OPT_BOOTSTRAP_CA_CERT,
        OPT_ENABLE_DUMMY,
        OPT_DISABLE_SYSTEM,
        DAEMON_OPTION_ENUMS,
        OPT_DPDK,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"version",     no_argument, NULL, 'V'},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {NULL, 0, NULL, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(OFP10_VERSION, OFP10_VERSION);
            exit(EXIT_SUCCESS);

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS
        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
} /* parse_options() */

static void
ledd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;
    unixctl_command_reply(conn, NULL);
} /* ledd_exit() */


/* set the "marked" value for each subsystem to false. */
static void
ledd_unmark_subsystems(void)
{
    struct shash_node *node;

    SHASH_FOR_EACH(node, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)node->data;
        subsystem->marked = false;
    }
} /* ledd_unmark_subsystems() */



/* ************ OVS  ******************** */

/* perform general initialization, including registering for notifications */
static void
ledd_init(const char *remote)
{
    int retval;

    /* initialize subsystems */
    init_subsystems();

    /* initialize the yaml handle */
    yaml_handle = yaml_new_config_handle();

    idl = ovsdb_idl_create(remote, &ovsrec_idl_class, false, true);
    idl_seqno = ovsdb_idl_get_seqno(idl);
    ovsdb_idl_set_lock(idl, "ops_ledd");
    /* Commenting this out to allow read/write for state column. */
    /* ovsdb_idl_verify_write_only(idl); */

    /* register interest in daemon table */
    ovsdb_idl_add_table(idl, &ovsrec_table_daemon);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_cur_hw);
    ovsdb_idl_omit_alert(idl, &ovsrec_daemon_col_cur_hw);

    /* register interest in all led columns */
    ovsdb_idl_add_table(idl, &ovsrec_table_led);
    ovsdb_idl_add_column(idl, &ovsrec_led_col_id);
    ovsdb_idl_omit_alert(idl, &ovsrec_led_col_id);
    ovsdb_idl_add_column(idl, &ovsrec_led_col_state);
    ovsdb_idl_add_column(idl, &ovsrec_led_col_status);
    ovsdb_idl_omit_alert(idl, &ovsrec_led_col_status);

    /* register interest in the subsystems. this process needs the
       name and hw_desc_dir fields. the name value must be unique within
       all subsystems (used as a key). the hw_desc_dir needs to be populated
       with the location where the hardware description files are located */
    ovsdb_idl_add_table(idl, &ovsrec_table_subsystem);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_other_config);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_hw_desc_dir);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_leds);
    ovsdb_idl_omit_alert(idl, &ovsrec_subsystem_col_leds);

    unixctl_command_register("ops-ledd/dump", "", 0, 0,
                             ledd_unixctl_dump, NULL);

    retval = event_log_init("LED");

    if(retval < 0) {
         VLOG_ERR("Event log initialization failed for LED");
    }
} /* ledd_init() */

struct ovsrec_led *
lookup_led(const char *name)
{
    const struct ovsrec_led *led;

    OVSREC_LED_FOR_EACH(led, idl) {
        if (strcmp(led->id, name) == 0) {
            return((struct ovsrec_led *)led);
        }
    }

    return(NULL);
} /* lookup_led() */

/************************************************************************//**
 * Function that looks to see if the user has
 *     changed the desired state of any LED and then processes the request
 *
 * Logic:
 *   foreach LED in this subsystem
 *       find the matching entry in the LED table in ovsdb
 *       if the state has changed   (User requested a state change)
 *           set the LED to the new state
 *           update the LED status in ovsdb, if changed
 *
 * Returns:  void
 ***************************************************************************/
void
process_changes_in_subsys(struct locl_subsystem *subsys)
{
    const struct ovsrec_led *ovs_led;
    struct locl_led *led;
    struct shash_node *node;

    /* If we were unable to process the hwdesc file for this subsys, return. */
    if (subsys->subsys_status == LEDD_SUBSYS_STATUS_IGNORE) {
        VLOG_DBG("subsys %s set to IGNORE",subsys->name);
        return;
    }

    /* foreach led in this subsystem... */
    SHASH_FOR_EACH(node, &(subsys->subsystem_leds)) {
        led = (struct locl_led *)node->data;

        /* foreach entry in the LED table */
        OVSREC_LED_FOR_EACH(ovs_led, idl) {
            /* If they don't match, continue. */
            if (strcmp(led->name, ovs_led->id) != 0) {
                continue;
            }

            /* If a new state has been written into the db, process it. */
            if (led->state != ledd_state_to_enum(ovs_led->state)) {
                enum ovsrec_led_status_e status;

                led->state = ledd_state_to_enum(ovs_led->state);

                /* If we have a valid type, write to the LED */
                if (ledd_get_led_type(subsys, led->yaml_led->type) !=
                                        (YamlLedType *) NULL) {

                    if (ledd_write_led(subsys, led)) {
                        VLOG_DBG("ledd_write successful, %s",led->name);
                        status = LED_STATUS_OK;
                    } else {
                        VLOG_WARN("ledd_write failed, %s",led->name);
                        status = LED_STATUS_FAULT;
                    }
                } else {
                    VLOG_WARN("Unable to write LED %s, led type %s unknown",
                            led->name, led->yaml_led->type);
                    status = LED_STATUS_FAULT;
                }

                /* If there is a new status, push it to the db. */
                if (ledd_status_to_enum(ovs_led->status) != status) {
                    ovsrec_led_set_status(ovs_led,
                         ledd_status_to_string(status));
                    change_to_commit = true;
                }
            }
        }
        subsys->marked = true;
    }

} /* process_changes_in_subsys() */

/************************************************************************//**
 * Function that creates a new locl_subsystem structure
 *     when a new subsystem is found in ovsdb, sets the LEDs to their
 *     default values, and adds the LEDs into the ovsdb led table.
 *
 * Logic:
 *      - create a new locl_subsystem structure, add to hash
 *      - tag the subsystem as "unmarked" and as IGNORE
 *      - extract the LED information for this subsys from the hw desc files.
 *        This includes names and types of LEDs, and their supported
 *        states and settings.
 *      - foreach valid led
 *          - write the default value to the LED
 *          - add the LED to the LED table (add to transaction)
 *      - tag the subsystem as "marked" and as OK
 *      - set change_to_commit = true  (transaction has changes to be pushed)
 *
 * Returns:  void
 ***************************************************************************/
void
add_subsystem(const struct ovsrec_subsystem *ovsrec_subsys,
                            struct ovsdb_idl_txn *txn)
{
    struct locl_subsystem *lsubsys;
    int rc;
    int type_count;
    int idx;
    int led_count;
    struct ovsrec_led **led_array;
    const char *dir;
    const YamlLedInfo *led_info;

    VLOG_DBG("Adding new subsystem %s", ovsrec_subsys->name);

    lsubsys = (struct locl_subsystem *)malloc(sizeof(struct locl_subsystem));
    memset(lsubsys, 0, sizeof(struct locl_subsystem));

    (void)shash_add(&subsystem_data, ovsrec_subsys->name, (void *)lsubsys);

    lsubsys->name = strdup(ovsrec_subsys->name);
    lsubsys->marked = false;
    lsubsys->subsys_status = LEDD_SUBSYS_STATUS_IGNORE;
    lsubsys->parent_subsystem = NULL;  /* OPS_TODO: find parent subsystem */

    shash_init(&lsubsys->subsystem_leds);
    shash_init(&lsubsys->subsystem_types);

    /* use a default if the hw_desc_dir has not been populated */
    dir = ovsrec_subsys->hw_desc_dir;

    if (dir == NULL || strlen(dir) == 0) {
        VLOG_ERR("No h/w description directory for subsystem %s",
                                    ovsrec_subsys->name);
        return;
    }

    /* since this is a new subsystem, load all of the hardware description
       information about the LEDs (just for this subsystem).
       parse LED and device data for subsystem */
    rc = yaml_add_subsystem(yaml_handle, ovsrec_subsys->name, dir);

    if (rc != 0) {
        VLOG_ERR("Error processing h/w description files for subsystem %s",
                                    ovsrec_subsys->name);
        return;
    }

    rc = yaml_parse_devices(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s devices file (in %s)",
                                ovsrec_subsys->name, dir);
        return;
    }

    rc = yaml_parse_leds(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s led file (in %s)",
                                ovsrec_subsys->name, dir);
        return;
    }

    led_info = yaml_get_led_info(yaml_handle, ovsrec_subsys->name);

    if (led_info == NULL) {
        VLOG_INFO("subsystem %s has no LED info", ovsrec_subsys->name);
        return;
    }

    /* get the # of LED types */
    lsubsys->num_types =
        yaml_get_led_type_count(yaml_handle, ovsrec_subsys->name);
    type_count = led_info->number_types;

    /* get the # of LEDs found in the yaml file. */
    lsubsys->num_leds = yaml_get_led_count(yaml_handle, ovsrec_subsys->name);
    led_count = led_info->number_leds;

    if ( (lsubsys->num_leds <= 0) || (lsubsys->num_types <= 0) ) {
        return;
    }

    /* Verify that the type # specified and # found are the same. */
    if (lsubsys->num_types != type_count) {
        VLOG_WARN("LED type count does not match in %s/led.yaml file. Info "
                "says it is %d, while the number counted in the file is %d",
                dir, type_count, lsubsys->num_types);
        type_count = lsubsys->num_types;
    }
    else {
        VLOG_DBG("There are %d LED types in subsystem %s", type_count,
                                 ovsrec_subsys->name);
        log_event("LED_COUNT", EV_KV("count", "%d", type_count),
            EV_KV("subsystem", "%s", ovsrec_subsys->name));
    }

    /* Verify that the LED # specified and # found are the same. */
    if (lsubsys->num_leds != led_count) {
        VLOG_WARN("LED count does not match in %s/led.yaml file. Info says "
                "it is %d, while the number counted in the file is %d",
                dir, led_count, lsubsys->num_leds);
        led_count = lsubsys->num_leds;
    }
    else {
        VLOG_DBG("There are %d LEDs in subsystem %s", led_count,
                                 ovsrec_subsys->name);
    }

    led_array = (struct ovsrec_led **)
                xcalloc(led_count, sizeof(struct ovsrec_led *));

    /* Add the types to the locl_subsystem structure */
    for (idx = 0; idx < (int) type_count; idx++) {
        size_t i;
        bool found = false;
        const YamlLedType *new_type;

        new_type = yaml_get_led_type(yaml_handle, ovsrec_subsys->name, idx);

        if (new_type == (YamlLedType *) NULL) {
            VLOG_ERR("subsystem %s had error reading LED type",
                     ovsrec_subsys->name);
            continue;
        }

        /* See if this is a type we know about. */
        for (i = 0; i < sizeof(led_type_strings)/sizeof(const char *);
                                                i++) {
            if (strcmp(led_type_strings[i], new_type->type) == 0) {
                found = true;
                break;
            }
        }

        /* If we know about it, add it. */
        if (found) {
            shash_add(&(lsubsys->subsystem_types), new_type->type,
                                    (void *)new_type);
        } else {
            VLOG_DBG("unknown type %s specified in %s", new_type->type, dir);
        }
    }

    /* walk through LEDs and add them to DB */
    for (idx = 0; idx < led_count; idx++) {
        struct ovsrec_led *ovs_led;
        char *led_name = NULL;
        const YamlLed *led;
        struct locl_led *new_led;
        YamlLedType *led_type;

        led = yaml_get_led(yaml_handle, ovsrec_subsys->name, idx);

        VLOG_DBG("Adding LED %s in subsystem %s", led->name,
                                        ovsrec_subsys->name);

        /* Create the new locl led struct and initialize it. */
        asprintf(&led_name, "%s-%s", ovsrec_subsys->name, led->name);
        new_led = (struct locl_led *)malloc(sizeof(struct locl_led));
        new_led->name = led_name;
        new_led->subsystem = lsubsys;
        new_led->yaml_led = led;
        new_led->state = LED_STATE_OFF;
        new_led->status = LED_STATUS_OK;

        led_type = ledd_get_led_type(lsubsys, led->type);
        if (led_type == NULL) {
            new_led->settings = (YamlLedTypeSettings *)NULL;
            new_led->status = LED_STATUS_FAULT;
        } else {
            new_led->settings = &(led_type->settings);
        }

        /* Add this new locl led to the led shash in subsystem shash */
        shash_add(&lsubsys->subsystem_leds, led->name, (void *)new_led);

        /* look for existing LED rows */
        ovs_led = lookup_led(led_name);

        /* If it isn't in ovsdb, then add it. */
        if (ovs_led == NULL) {
            ovs_led = ovsrec_led_insert(txn);

            /* Add to ovsdb. */
            ovsrec_led_set_id(ovs_led, led_name);
            ovsrec_led_set_state(ovs_led,
                    ledd_state_to_string(new_led->state));
        }

        /* Write the LED */
        if (ledd_write_led(lsubsys, new_led)) {
            VLOG_DBG("ledd_write successful, %s",led->name);
            new_led->status = LED_STATUS_OK;
        } else {
            VLOG_WARN("ledd_write failed, %s",led->name);
            new_led->status = LED_STATUS_FAULT;
        }

        /* Either way, set that status accordingly. */
        ovsrec_led_set_status(ovs_led,
                    ledd_status_to_string(new_led->status));

        led_array[idx] = ovs_led;
    }

    /* Push the data to the DB. */
    ovsrec_subsystem_set_leds(ovsrec_subsys, led_array, led_count);
    change_to_commit = true;

    free(led_array);

    /* Update the state of the locl_subsystem structure */
    lsubsys->marked = true;
    lsubsys->subsys_status = LEDD_SUBSYS_STATUS_OK;

    return;
} /* add_subsystem() */

/************************************************************************//**
 * Function that looks for changes in the OVSDB that need
 *     to be processed, either new or removed subsystems or changed
 *     configuration data.
 *
 * Logic:
 *     - initialize empty transaction
 *     - unmark all subsystems so removed subsystems can be detected.
 *     - foreach subsystem in ovsdb
 *        - if new_to_us, call add_subsystem
 *        - else call process_changes_in_subsys
 *     - if first_time_through_loop, set cur_hw_cfg = 1
 *     - if change_to_commit is true, submit the transaction
 *     - call ledd_remove_unmarked_subsystems to process (delete)
 *          any subsystems no longer in ovsdb
 *
 * Returns:  void
 ***************************************************************************/
static void
ledd_reconfigure(void)
{
    const struct ovsrec_subsystem *ovs_sub;
    const struct ovsrec_daemon *ovs_daemon;
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);
    struct ovsdb_idl_txn *txn;

    COVERAGE_INC(ledd_reconfigure);

    if (new_idl_seqno == idl_seqno){
        return;
    }

    /* Unmark all subsystems so we can tell if any have been removed. */
    ledd_unmark_subsystems();

    change_to_commit = false;
    txn = ovsdb_idl_txn_create(idl);

    /* For each subsystem in ovsdb, process it (add or update) */
    OVSREC_SUBSYSTEM_FOR_EACH(ovs_sub, idl) {
        struct locl_subsystem *subsystem;

        subsystem = shash_find_data(&subsystem_data, ovs_sub->name);

        if (subsystem == NULL) {
            /* If the subsystem is new, add it */
            add_subsystem(ovs_sub, txn);
        } else {
            /* Else, look for any changes to process */
            process_changes_in_subsys(subsystem);
        }
    }

    idl_seqno = new_idl_seqno;

    /* Set cur_hw = 1 if this is first time through. */
    if (!cur_hw_set) {
        OVSREC_DAEMON_FOR_EACH(ovs_daemon, idl) {
            if (strncmp(ovs_daemon->name, NAME_IN_DAEMON_TABLE,
                        strlen(NAME_IN_DAEMON_TABLE)) == 0) {
                ovsrec_daemon_set_cur_hw(ovs_daemon, (int64_t) 1);
                cur_hw_set = true;
                change_to_commit = true;
                break;
            }
        }
    }

    /* If there are changes for ovsdb, submit the transaction. */
    if (change_to_commit) {
        ovsdb_idl_txn_commit_block(txn);
    }
    ovsdb_idl_txn_destroy(txn);

    /* For any missing subsystems (no longer there), remove them. */
    ledd_remove_unmarked_subsystems();

} /* ledd_reconfigure() */

static void
ledd_run(void)
{
    ovsdb_idl_run(idl);

    if (ovsdb_idl_is_lock_contended(idl)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

        VLOG_ERR_RL(&rl, "another ops-ledd process is running, "
                    "disabling this process until it goes away");

        return;
    } else if (!ovsdb_idl_has_lock(idl)) {
        return;
    }

    ledd_reconfigure();

    daemonize_complete();
    vlog_enable_async();
    VLOG_INFO_ONCE("%s (OpenSwitch ledd) %s", program_name, VERSION);
} /* ledd_run() */

static void
ledd_wait(void)
{
    ovsdb_idl_wait(idl);
} /* ledd_wait() */

/* ************ MAIN ******************** */
int
main(int argc, char *argv[])
{
    char *unixctl_path = NULL;
    struct unixctl_server *unixctl;
    char *remote;
    bool exiting;
    int retval;

    set_program_name(argv[0]);

    proctitle_init(argc, argv);
    remote = parse_options(argc, argv, &unixctl_path);
    fatal_ignore_sigpipe();

    ovsrec_init();

    daemonize_start();

    retval = unixctl_server_create(unixctl_path, &unixctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 0, ledd_exit, &exiting);

    ledd_init(remote);
    free(remote);

    exiting = false;
    while (!exiting) {
        ledd_run();
        unixctl_server_run(unixctl);

        ledd_wait();
        unixctl_server_wait(unixctl);
        if (exiting) {
            poll_immediate_wake();
        }
        poll_block();
    }

    ovsdb_idl_destroy(idl);
    unixctl_server_destroy(unixctl);

    return 0;
} /* main() */
