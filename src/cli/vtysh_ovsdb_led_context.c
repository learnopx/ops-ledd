/* Led daemon client callback resigitration source files.
 *
 * Copyright (C) 2016 Hewlett Packard Enterprise Development LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * File: vtysh_ovsdb_led_context.c
 *
 * Purpose: Source for registering led sub-context callback with
 *          global config context.
 */

#include "vtysh/vty.h"
#include "vtysh/vector.h"
#include "vswitch-idl.h"
#include "openswitch-idl.h"
#include "vtysh/vtysh_ovsdb_if.h"
#include "vtysh/vtysh_ovsdb_config.h"
#include "vtysh/utils/system_vtysh_utils.h"
#include "vtysh_ovsdb_led_context.h"

#define DEFAULT_LED_STATE OVSREC_LED_STATE_OFF

/***************************************************************************
* @function      : vtysh_config_context_led_clientcallback
* @detail    : client callback routine for LED configuration
* @parame[in]
*   p_private: Void pointer for holding address of vtysh_ovsdb_cbmsg_ptr
*          structure object
* @return : e_vtysh_ok on success
***************************************************************************/
vtysh_ret_val
vtysh_config_context_led_clientcallback(void *p_private)
{
    vtysh_ovsdb_cbmsg_ptr p_msg = (vtysh_ovsdb_cbmsg *)p_private;
    const struct ovsrec_led *pLedRow = NULL;

    OVSREC_LED_FOR_EACH(pLedRow,p_msg->idl)
    {
        if(pLedRow)
        {
            /* Assuming there is no misconfiguration,
             * state can be on|off|flashing */
            if(0 != strcasecmp(pLedRow->state,DEFAULT_LED_STATE))
            {
                vtysh_ovsdb_cli_print(p_msg,"%s %s %s", "led",
                                      pLedRow->id,pLedRow->state);
            }
        }
    }

    return e_vtysh_ok;
}
