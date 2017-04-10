# -*- coding: utf-8 -*-

# (c) Copyright 2015 Hewlett Packard Enterprise Development LP
#
# GNU Zebra is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
#
# GNU Zebra is distributed in the hope that it will be useful, but
# WITHoutput ANY WARRANTY; withoutput even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Zebra; see the file COPYING.  If not, write to the Free
# Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
# 02111-1307, USA.


TOPOLOGY = """
# +-------+
# |  sw1  |
# +-------+

# Nodes
[type=openswitch name="Switch 1"] sw1
"""


def init_led_table(sw1):
    # Add dummy data for LED in subsystem and led table for simulation.
    # Assume there would be only one entry in subsystem table
    output = sw1('list subsystem', shell='vsctl')
    lines = output.split('\n')
    for line in lines:
        if '_uuid' in line:
            _id = line.split(':')
            uuid = _id[1].strip()
            sw1('ovs-vsctl -- set Subsystem {} '
                ' leds=@led1 -- --id=@led1 create led '
                ' id=base1 state=flashing status=ok'.format(uuid),
                shell='bash')


def led_on(sw1):
    is_led_set = False
    output = sw1('configure terminal')
    output = sw1('led base1 on')
    sw1('exit')
    output = sw1('list led base1', shell='vsctl')
    lines = output.split('\n')
    for line in lines:
        if 'state' in line:
            if 'on' in line:
                is_led_set = True
                break
    assert is_led_set is True


def show_led(sw1):
    led_config_present = False
    output = sw1('show system led')
    lines = output.split('\n')
    for line in lines:
        if 'base1' in line:
            if 'on' in line:
                led_config_present = True
                break
            elif 'off' in line:
                led_config_present = True
                break
            elif 'flashing' in line:
                led_config_present = True
                break
            else:
                led_config_present = False
    assert led_config_present is True


def led_off(sw1):
    led_state_off = False
    output = sw1('configure terminal')
    output = sw1('no led base1')
    sw1('exit')
    output = sw1('list led base1', shell='vsctl')
    lines = output.split('\n')
    for line in lines:
        if 'state' in line:
            if 'off' in line:
                led_state_off = True
                break
    assert led_state_off is True


def running_config_led(sw1):
    led_config_present = False
    output = sw1('configure terminal')
    output = sw1('led base1 on')
    sw1('exit')
    output = sw1('show running-config')
    lines = output.split('\n')
    for line in lines:
        if 'led base1 on' in line:
            led_config_present = True
            break
    assert led_config_present is True


def test_led_ct_led(topology, step):
    # Initialize the led table with dummy value
    sw1 = topology.get("sw1")
    init_led_table(sw1)
    # led <led name> on|off|flashing test.
    step('Test to verify \'led\' command')
    led_on(sw1)
    # show system led test.
    step('Test to verify \'show system led\' command')
    show_led(sw1)
    # no led <led name> test
    step('Test to verify \'no led\' command')
    led_off(sw1)
    # no led show running-config test
    step('Test to verify show running-config command')
    running_config_led(sw1)
