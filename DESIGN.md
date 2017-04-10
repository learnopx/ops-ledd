# High level design of ops-ledd
The ops-ledd daemon manages the LEDs for the platform.

## Responsibilities
ops-ledd reads and writes LEDs, as supported by each platform. Currently, the only writable LED is the "location" LED. This LED is under direct user control and can be turned on, off, or flashing. The purpose of the "location" LED is to physically locate a specific platform by identifying the platform with the lit "location" LED.

## Design choices
N/A

## Relationships to external OpenSwitch entities
```ditaa
  +----------+     +----------+
  | ops-ledd +---->+  OVSDB   |
  +----------+     +----------+
       |   |
       |   |       +-------+
       |   +------>| LEDs  |
       |           +-------+
       v
  +--------------------+
  |hw description files|
  +--------------------+
```

## OVSDB-Schema
The following rows/cols are created by ops-ledd
```
  rows in led table
  led:id
  led:state
  led:status
```

The following cols are written by ops-ledd
```
  led:status
  daemon["ops-ledd"]:cur_hw
  subsystem:leds
```

The following cols are read by ops-ledd
```
  led:state
  subsystem:name
  subsystem:hw_desc_dir
```

## Internal structure
### Main loop
Main loop pseudo-code
```
  initialize OVS IDL
  initialize appctl interface
  while not exiting
  if db has been configured
     check for any inserted/removed LEDs
     for each LED
        if change
           update status
           write LED
  check for appctl
  wait for IDL or appctl input
```

### Source files
```ditaa
  +---------+
  | ledd.c  |       +---------------------+
  |         |       | config-yaml library |    +----------------------+
  |         +------>+                     +--->+ hw description files |
  |         |       |                     |    +----------------------+
  |         |       |                     |
  |         |       |            +--------+
  |         +------------------> | i2c    |    +------+
  |         |       |            |        +--->+ LEDs |
  |         |       +------------+--------+    +------+
  +---------+
```

### Data structures
```
locl_subsystem: list of LEDs and their status
locl_led: LED data
```

## References
* [config-yaml library](/documents/dev/ops-config-yaml/DESIGN)
