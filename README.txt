TinyTx - RP2040 Zero to CRSF / ExpressLRS bridge
=======================================================

Converts a USB game controller, flight stick, RC radio in USB joystick mode,
SBUS receiver, PPM stream or ELRS receiver into a CRSF transmitter for an
ExpressLRS module.


INSTALLING
----------
1. Copy the whole TinyTx folder into your Arduino sketchbook, or open
   TinyTx.ino directly. All files must sit in the same folder.

2. Board support - add this URL under File > Preferences >
   "Additional boards manager URLs":

     https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

   then install "Raspberry Pi Pico/RP2040" by earlephilhower.

3. THREE MANDATORY SETTINGS under Tools:

     Board        Raspberry Pi RP2040 Boards > Waveshare RP2040 Zero
     USB Stack    Adafruit TinyUSB Host        <-- must be the HOST variant
     Flash Size   2MB (Sketch: 1MB, FS: 1MB)   <-- needed for device profiles

   Plain "Adafruit TinyUSB" is device mode and will detect nothing.
   A Flash Size with no FS partition means profiles cannot be saved.

4. Libraries (Sketch > Include Library > Manage Libraries):

     Adafruit TinyUSB Library
     Adafruit SSD1306
     Adafruit GFX Library
     Adafruit NeoPixel

   If two copies of Adafruit TinyUSB are installed, delete the one under
   Documents/Arduino/libraries so the version bundled with the core is used.

5. Uploading: the USB port is in host mode, so no COM port appears while the
   firmware runs. Hold BOOTSEL and tap RESET, then upload as normal or drag
   the .uf2 onto the RPI-RP2 drive.


WIRING
------
  GP0        CRSF TX    to the ELRS module signal pin through 100 ohm
  GP1        CRSF RX    tied to GP0 for single-wire JR modules
  GP4 / GP5  OLED SDA / SCL, 128x64 SSD1306 (tries 0x3C then 0x3D at boot)
  GP6        UP button
  GP7        DOWN button
  GP10       OK button      (select / confirm / enter edit mode)
  GP11       BACK button    (cancel / go back)
  GP13       SBUS in (inverted) or ELRS receiver CRSF in
  GP14       PPM in
  GP16       NeoPixel
  USB-C      USB host - controllers plug in here

  UP/DOWN stay on GP6/GP7; OK/BACK moved off GP8/GP9 to free that pin pair
  for the DFPlayer's UART1 link (see the DFPLAYER section below), landing on
  GP10/GP11 instead.

THE 4-BUTTON MENU
------------------
The whole menu is driven from four buttons: UP, DOWN, OK and BACK.

  UP / DOWN   Move the cursor. On a screen with a ranged value open for
              editing (weight, trim, deadband, ms, channel counts...),
              UP/DOWN change the value instead. Both auto-repeat while
              held. On a YES/NO dialog they toggle the selection.

  OK          Select / enter a submenu / confirm a dialog. On a row with
              a ranged value, a short press opens it for editing; press
              OK again (or BACK) to close it and keep the new value. On
              simple ON/OFF or cycle-through settings, OK just toggles
              or advances it directly - no edit mode needed.

  BACK        Cancel / go up one menu level. On a dialog it always
              backs out without applying anything. While a value is
              open for editing, BACK closes the row (same as OK there).

  Long-press OK   A few rows have no direct on-screen "clear" option
                  (e.g. an assigned axis/button on a mix slot, or a
                  trim-step button binding). Holding OK on that row
                  clears it. The footer hints at this with "Hold: clear"
                  or similar wherever it applies.

  Long-press BACK   On the main Status screen, jumps straight to the
                  Profiles list regardless of which carousel page is
                  showing - see PROFILES below. While entering a module
                  parameter's text value (ELRS Setup's character-wheel
                  editor), bails out to the parameter list instead of
                  working backwards one character at a time. Unused
                  everywhere else.

Everywhere the menu used to rely on a two-option LEFT/RIGHT toggle (mode
switch confirmations, delete confirmations, save confirmation, etc.) that
is now a dedicated YES/NO dialog screen you step through with UP/DOWN and
confirm with OK - nothing to memorize per-screen, it behaves the same way
everywhere.

WIRING THE ELRS MODULE
----------------------
Menu > Link Setup > Wiring selects one of three arrangements. Changing it
reboots, because the receiver has to be re-plumbed.

  "2 pin 1wire"   RECOMMENDED - receive uses the hardware UART

      GP0 TX --+
               +--[100R]--- signal --- ELRS module
      GP1 RX --+
      GND ---------------- GND

    Still a single wire to the module; the second pin is only a jumper on the
    board. This is the dependable arrangement.

  "1 pin GP0"     single pin, single wire - relies on a PIO receiver

      GP0 ------------- signal --- ELRS module
      GND ------------- GND

    GP1 is not used at all. The hardware UART transmits on GP0 while a PIO
    receiver samples the same pad, which works because the RP2040 routes a
    pin's input to every peripheral no matter which one is driving the pad.
    Between frames GP0 is released to a high-impedance input so the module
    can drive the line to reply.

  "2 pin TX/RX"   full duplex, separate lines

      GP0 ------------- module RX
      GP1 ------------- module TX
      GND ------------- GND

Transmit and receive polarity are separate. Transmit is the Invert setting and
is never changed automatically - flipping it would disconnect a module that is
already accepting frames, which is audible as the module beeping in and out.

Receive polarity is searched on its own: it alternates every 1.5 s until a
frame decodes, then keeps what worked. The Invert row reads

    Invert N rxY~

meaning transmit normal, receive inverted, search still running. The "~"
disappears once a frame has been decoded.

CRSF to an ExpressLRS module is plain 420 kbaud UART, idle high - not the
inverted S.Port signalling used by some other links - so Invert normally
settles on NO. The line
above the footer reads "Module replying OK" the moment a valid frame comes
back, so both settings can be judged without leaving the page.


SAFE MODE
---------
Hold BACK while powering on to boot with the USB host disabled. If a device
ever makes the board reset repeatedly, this is the way back in: the menu works
normally so settings can be changed, and USB stays off until the next restart.


STATUS LED
----------
The NeoPixel on GP16 reflects overall state at a glance:

  Magenta   The OLED never ACKed on the I2C bus at boot, at either 0x3C or
            0x3D (checked directly with the bus, not just by asking the
            display library - see below). The firmware runs headless in
            this state - buttons, CRSF/PPM output, special functions and
            sound all keep working - but there is no way to reach the menu.
            This points at wiring/power to the panel rather than firmware:
            SDA/SCL swapped or not actually connected, no 3.3V/GND to the
            module, or missing pull-ups on a bare panel. See WIRING above.
  Red       No ELRS module detected (or, briefly at boot, a watchdog reboot
            just occurred - three quick red flashes).
  Orange    Failsafe active, or link quality has dropped below 40.
  Blue      No control input detected (nothing connected/bound).
  Green     Everything normal - module found, input present, link healthy.

Magenta takes priority over all of the above, since with no screen you would
otherwise have no clue why the LED was stuck on any of the other colours.

If the LED is NOT magenta (something did ACK on the bus) but the screen is
still blank: the display chip is present and answering, so this is no longer
a wiring problem, but a chip/driver mismatch. The two common causes are (1)
the panel actually uses an SH1106 controller rather than SSD1306 - very
common on cheap "0.96 inch 128x64" boards, they look identical and both
answer at 0x3C, but SH1106 needs a different init/addressing sequence and
will stay dark under an SSD1306 driver - or (2) the panel needs
SSD1306_EXTERNALVCC instead of SSD1306_SWITCHCAPVCC (a jumper/resistor
difference on some clone boards that disables the internal charge pump).
Mention which of these it is if you can identify the panel and it can be
special-cased in config.h.


READING CHANNELS FROM A SECOND MICROCONTROLLER
------------------------------------------------
GP13 (PIN_AUX_RX) is a second UART, completely separate from the CRSF link
on GP0/GP1 that talks to the ELRS module. It already doubles as a generic
channel input and needs no firmware changes to use it that way - pick
whichever framing is easier to produce on the other microcontroller:

  Menu > Device Mode > "CRSF Aux In"   (DeviceMode::ELRS_REPEATER)

    Wire the other microcontroller's UART TX to GP13, GND to GND. It
    normally listens to an ELRS receiver here, but it decodes any valid
    CRSF RC-channels frame the same way, from any sender:

      Baud            420000 8N1, NOT inverted
      Address byte    0xC8 (CRSF_ADDR_FC) or 0x00 (broadcast)
      Length byte     payload length + 2 (24 for a full 16-channel frame)
      Frame type      0x16 (RC_CHANNELS_PACKED)
      Payload         16 channels, 11 bits each, little-endian packed,
                       centre = 992, range roughly 172..1811
      Last byte       CRC-8 (poly 0xD5) over [type + payload]

    This is the standard CRSF RC-channels frame used throughout the ELRS
    ecosystem - any library that can build one (e.g. an existing CRSF
    Arduino/ESP library) works here unmodified. See crsf.cpp/crsfCRC8()
    and the CRSF_ADDR_*/CRSF_FRAMETYPE_* defines in crsf.h for the exact
    byte layout if you are rolling your own.

  Menu > Device Mode > "SBUS In"   (DeviceMode::SBUS_INPUT)

    Simpler to bit-bang from a microcontroller that already has an SBUS
    library. Same pin, GP13:

      Baud            100000 8E2, inverted (idle low)
      Frame           25 bytes: 0x0F header, 16x11-bit channels
                       (little-endian packed, centre = 1024), a flags
                       byte (bit 3 = failsafe, bit 2 = frame lost),
                       0x00 footer

  Menu > Device Mode > "Pot/Btn UART"   (DeviceMode::POT_BTN_UART)

    For a second microcontroller reading potentiometers and buttons
    directly rather than emulating an RC receiver protocol - the pairing
    for this mode is the "rp2040_zero_pot_button_uart_binary" sketch (an
    RP2040-Zero reading up to 4 ADS1115 pots plus up to 4 onboard ADC
    pots, and up to 32 buttons, over its own UART0), though anything that
    emits the same packet works identically. Wire its UART TX to GP13,
    GND to GND - it is transmit-only, same as the other two options here.

      Baud     115200 8N1, not inverted
      Frame    SYNC1 (0xAA) SYNC2 (0x55) NUM_POTS NUM_BUTTONS
               then NUM_POTS x uint16 pot values, little-endian, full
               0..65535 scale (mapped straight onto TinyTx's usual
               -32768..32767 axis range - calibration works the same as
               any other source, so it does not need to be centred)
               then ceil(NUM_BUTTONS/8) bytes of packed button bits
               (bit i%8 of byte i/8 = button i, 1 = pressed)
               then an 8-bit sum checksum over every byte from NUM_POTS
               through the end of the button bitfield (not the two sync
               bytes)

    The packet is self-describing - NUM_POTS and NUM_BUTTONS are read
    from the packet itself each time, up to TinyTx's limits of 16 axes
    and 32 buttons, so the sender's pot/button count doesn't need to
    match any compile-time setting on this end.

Any of these three modes reports its axes and buttons through the normal
channel list / mixer / failsafe / channel-learning screens exactly like a
USB gamepad would - nothing about mixing or channel setup changes based on
where the axes came from.


EXPRESSLRS ELRS SETUP MENU
---------------------------
Menu > ELRS Setup is a full equivalent of the elrs.lua script that runs on
a radio's own screen, built for TinyTx's 4 buttons and 128x64 display. It
does the same thing Lua does: ask the connected module what its menu
actually looks like right now, rather than assume anything in advance.

  - Opening the menu walks the module's entire live parameter tree over
    CRSF (DEVICE_INFO for the field count, then PARAMETER_READ/
    PARAMETER_SETTINGS_ENTRY for every field), the same discovery a Lua
    script performs. Nothing is hardcoded, so it's automatically correct
    for whatever firmware version is on the module - including new fields
    a future ExpressLRS release adds, without needing a TinyTx update to
    know about them.
  - Folders (e.g. "VTX Admin") navigate exactly like a submenu: OK opens
    one, BACK steps back out one level at a time, all the way to the menu.
  - A plain list of choices (Packet Rate, TX Power, and so on) opens as a
    scrollable list - UP/DOWN pick one, OK sends it.
  - A ranged number (including the 16- and 32-bit fields ExpressLRS 4.1
    added support for) opens for editing - UP/DOWN step it within the
    module's own min/max, OK sends it, BACK cancels.
  - A free-text field opens a character-wheel editor - UP/DOWN cycle the
    highlighted character, OK moves to the next one, holding OK sends it,
    BACK steps left (or cancels outright from the first character).
  - A command (Bind, WiFi Update, Reset, ...) asks for confirmation, then
    shows the module's own live status text while it runs - "Confirmed",
    "Timed out", etc. - the same as Lua's status line.
  - A read-only info field (firmware version, regulatory domain, ...) is
    just displayed.

Because every field is addressed by the name and id the module reports
together, in the moment, there's no fixed table that a firmware update
could silently make point at the wrong setting.

Not implemented: ExpressLRS 4.1's "Set Bindphrase From Handset" is sent
over a separate MSP-over-CRSF channel rather than the standard parameter
protocol above, and its exact message format isn't documented anywhere I
could verify while building this - so rather than guess and risk silently
sending something malformed, TinyTx doesn't attempt that specific path.
That said, if a module exposes its bind phrase as an ordinary STRING field
in its normal menu tree (some do), the free-text editor above will find
and edit it like any other field. Otherwise, setting it from the module's
WiFi page (or flashing it in) remains the reliable route.


VOICE ANNOUNCEMENTS (DFPLAYER)
--------------------------------
TinyTx can speak a handful of its own events through a DFPlayer Mini-
compatible MP3 module - the same module, the same wiring convention, and
the same SD card as the OpenI6X firmware's DFPlayer mod
(github.com/OpenI6X/opentx, wiki "Modifications > DFPlayer - Voice
announcements").

  Wiring:
    GP8  ----> DFPlayer RX
    GP3  <---- DFPlayer BUSY   (LOW while a track is playing)
    5V/3V3, GND as appropriate for your module
    Speaker on SPK1/SPK2, or a powered speaker on DAC_L/DAC_R

  Just like OpenI6X, this is a one-way link: TinyTx only ever sends
  commands, and reads the module's BUSY pin instead of its serial reply to
  know when a track finishes. No DFPlayer TX wire is needed.

  Files: copy them to the SD card's root, in the order their numeric
  prefix implies - the DFPlayer addresses "track N" by the Nth file
  written to the card, not by parsing the filename, so copy order is what
  actually matters and the prefix is only there to stop a file manager
  from silently reordering them. If you already have an SD card set up
  for an OpenI6X radio, it works here unmodified.

  The 13 built-in events default to these tracks (the same sounds as the
  OpenI6X stock English pack, renumbered for a compact card):

    0114_hello.wav     startup
    0116_telemko.wav   link lost / failsafe
    0115_telemok.wav   link established
    0117_thralert.wav  throttle not at idle at power-on
    0118_siglow.wav    link quality dropping (below 70%)
    0119_sigcrt.wav    link quality critical (below 50%)
    0120_maxtrim.wav   any channel's trim reaches +100%
    0121_mintrim.wav   any channel's trim reaches -100%
    0122_midtrim.wav   any channel's trim returns to centre
    0123_inactiv.wav   inactivity alarm (see below)
    0111_minute.wav    the word "minute" (timer minute call, count = 1)
    0112_minutes.wav   the word "minutes" (timer minute call, count != 1)
    0113_timovr1.wav   a countdown timer reaching zero

  Track layout: 1 = zero, 2-101 = 1 to 100, 102-110 = 200, 300 ... 1000,
  then the files above at 111-123. Your own sounds go from 124 onward.

  The last three back the per-timer Minute call described under TIMERS &
  SPECIAL FUNCTIONS: Menu > Timers > a timer > Minute call plays the spoken
  number (its own track, N for "N minutes left/elapsed") followed by
  whichever of these two words matches the count, and a countdown reaching
  zero plays the third. All three are ordinary rows in Sound Setup >
  Events, same as anything else here, so retrack them the same way if your
  pack numbers them differently.

  Trim buttons auto-repeat when held, like EdgeTX: one step on press, the
  first repeat after 0.4 s, then every 0.1 s, every 0.05 s after ten
  repeats. Crossing centre lands exactly on 0 and parks the repeat there
  until the button is released and pressed again.

  Each channel can switch those three trim sounds off: Channels > CHn >
  Trim > Sound (ON/OFF). It is saved with the profile.

  The three trim events fire for whichever channel just got there, however
  it got there - the trim buttons or hand-editing Trim in the menu - and
  don't say which channel, the same way a physical radio's trim beep
  doesn't either.

  The inactivity alarm fires once after no button press or stick/axis
  movement for a configured number of minutes, then again every further
  interval of the same length for as long as nothing moves - not just a
  one-shot reminder. Menu > Settings has Inactivity (on/off) and Idle mins
  (1-99) rows for it.

  Anything you copy after track 123 is free for your own sounds.

  Robustness: commands are spaced at least 150 ms apart (a second command
  sent while the first is still starting cuts it off, heard as a click).
  The volume is re-sent about 0.8 s after every controller connection,
  because a module that browned out and restarted when the controller drew
  its start-up current comes back at its own maximum volume. If BUSY is
  wired and the module never acknowledges a play command, it is reset and
  the track retried (twice at most). Power problems still need a hardware
  fix - a separate clean 5V and a bulk capacitor at the module.

  Menu > Settings has Voice (on/off) and Volume (0-30, the DFPlayer's own
  native range) rows.

  Menu > Sound Setup is where all of this is actually configured, and the
  track numbers above are only the pre-populated starting point:

    Events    One row per event listed above, showing its current track
              number. Selecting one opens it for editing - UP/DOWN step
              the track number by 1 and play it immediately as it
              changes, so you can dial in the right file by ear rather
              than by re-reading a list. OK plays it once more and
              confirms; 0 means that event stays silent.

    Triggers  Up to 6 independent "play this sound when..." slots, each
              bound to a controller button or axis, same as assigning a
              channel: select a slot, "Learn" it by pressing the button
              or moving the axis you want, then set what it plays. A
              button fires once per press. An axis fires once when it
              crosses a Threshold in the chosen Direction (Rising or
              Falling) - Learn picks a sensible starting Threshold from
              how far you moved it, both are adjustable afterwards. Hold
              OK on the Source row to clear a slot. These are independent
              of channel mixing - the same button or axis can drive a
              channel and a sound trigger at once, or a trigger can use
              an axis/button that isn't mixed into any channel at all.

  Either screen accepts any track number, not just the ones listed above -
  if you know the number of one of OpenI6X's ~270 stock sounds, or you've
  added your own past track 123, just dial it in the same way.


SPECIAL FUNCTIONS
-------------------
Menu > Special Func is an EdgeTX-style Special Functions setup, split the
same way EdgeTX splits it: Timers (plain objects, shown on the main
screen) and Functions (a switch-driven list that controls them, overrides
channels, and plays sounds).

  The overview page
    Status screen page 1 (UP/DOWN before the CH1-8 bars) is an EdgeTX-style
    main screen: the model name in double-size characters on the top row;
    below it, if a timer is set up, a small "TM1" label and the first timer
    as double-size M:SS (the colon blinks while it runs); four trim scales
    around the edge - CH3 on the left side, CH2 on the right side, CH4
    under the left stick box and CH1 under the right one - and the two
    stick boxes in the middle, laid out like a standard Mode 2 transmitter:
    left stick is Throttle (CH3, vertical) and Rudder (CH4, horizontal),
    right stick is Aileron (CH1, horizontal) and Elevator (CH2, vertical).
    The trim markers show each channel's configured Trim (-100..+100, same
    as Channels > CHn > Trim). "Model name" is whichever Profile most
    recently loaded for the attached controller (or was loaded by hand from
    Menu > Profiles) or just saved as a new slot, so a saved slot shows as
    "Slot 1", "Slot 2" and so on; with no profile loaded yet it shows the
    current Device Mode. Up to 9 characters are shown.

  Timers
    Up to 4 timers, each either Countdown (starts at a set MM:SS, up to
    99:59) or Count-up (always starts at 0:00). The first one you create
    (lowest-numbered slot, not necessarily the one most recently touched)
    shows up top-right of the Status screen's first page - see "The
    overview page" below. UP/DOWN on Status cycle through the whole
    carousel: the overview, CH1-8 bars, CH9-16 bars, an ELRS link
    statistics screen, every defined timer full-screen, then every
    diagnostics page (the same screens Menu > Input Info shows - USB
    info, raw report, live axis values) - all reachable directly from the
    main screen with no menu diving. A timer's view on this screen is read-only: watching
    it start, stop, or reset only ever happens through whatever
    button/switch a Special Function row was set up with (Start/Stop/
    Reset Timer) - there's no button on the Status screen itself for it,
    so a timer can't be nudged by accident while just checking on it. A
    countdown reaching zero always gets a spoken "timer over" call-out if a
    DFPlayer is wired up.

    Minute call (Timers > a timer > "Minute call", toggled with OK) is the
    same idea as EdgeTX's per-timer option of the same name: while on, each
    whole minute gets a spoken call-out too - elapsed minutes for a
    Count-up timer, minutes remaining for a Countdown - using the same
    spoken-number files (zero is track 1, so number N is track N+1), plus two more voice
    events ("Minute", "Minutes") editable from Menu > Sound Setup > Events
    just like Startup/Link up/etc - change their track numbers there if
    your SD card numbers those two words differently. On by default for
    every timer; switch it off per timer if you only want the zero/over
    call-out, which is itself also an editable event ("Timer over").

    The CH1-8/CH9-16 bars always reflect the actual output - after mixing
    and after any Override Channel row has had its say - and a small "o"
    appears under any channel currently being forced by one, so an active
    override is never silent or easy to forget about.

  The ELRS link statistics screen shows everything the standard CRSF
  LINK_STATISTICS frame carries that the one-line summary at the top of
  every other view doesn't already: RSSI on the second antenna, SNR,
  which antenna diversity is currently using, the TX power index the
  module reported (shown as a bare number rather than decoded to mW -
  what that index means is target- and version-specific enough that
  guessing at a table felt worse than not labelling it), and the
  downlink's own LQ/RSSI/SNR when the module sends telemetry back. RF
  mode is left off for the same reason.

  Functions
    A flat list of up to 12 rows. Selecting an empty slot creates a new
    one; selecting an existing row edits it, and holding OK on a row (or
    using its Delete row) removes it. Each row reads as one sentence, in
    this order:

        Trigger, Active or Inactive, Function, value(s)

    e.g. "Button 1, Active, Play Sound, 217" or "Button 2, Inactive,
    Reset Timer, 1". The first row, Source, picks what drives the row:
    Input (a button or axis) or Channel (a channel's output value, CH1-16).
    With Source = Channel the row shows Channel (which one), When (Above
    or Below) and Level (percent, -100 to +100, steps of 5) instead of the
    learned Trigger - e.g. "CH5 above 50%". For Volume only the channel is
    needed (its output maps to the min..max volume). Avoid choosing the
    very channel an Override Channel row forces, or it can hold itself on.
    With Source = Input, Trigger is learned the same way a channel's source is
    - move a stick or press a button. Active/Inactive is which state of
    that trigger fires the row: a button is Active while held, Inactive
    while released, so the same physical button can drive two rows - one
    for each state, e.g. an armed/disarmed pair of call-outs. An axis is
    Active on whichever side of its threshold you set, Inactive on the
    other. Function cycles through what the row does:

      Run Timer          level-based: the chosen timer counts for as long
                          as the row's condition holds and stops when it
                          doesn't - e.g. Source = Channel, CH5 above 0%,
                          Run Timer 1 counts while CH5 is high. A
                          countdown that reaches zero stays finished until
                          reset. Use either Run Timer or Start/Stop for a
                          given timer, not both.

      Override Channel   forces a channel to a set value for as long as
                          the row's condition holds (this is continuous
                          rather than one-shot - release the trigger and
                          the channel goes back to whatever the mixer
                          says). Never applies while the link is in
                          failsafe - a configured failsafe value for that
                          channel always wins over an active override, so
                          an override can't quietly replace the one thing
                          that's supposed to happen when the link is gone.
                          Any channel currently forced this way shows a
                          small "o" under its bar on the Status screen -
                          the bars always reflect the actual output, after
                          any override has had its say, not just the mix.
      Play Sound          plays a track once, the moment the condition
                          starts holding
      Set Timer           sets a timer's current value directly (doesn't
                          touch its configured starting value in the
                          Timers list - this is a runtime nudge, not a
                          reconfiguration, so it doesn't wear the flash)
      Start Timer         resumes a timer
      Stop Timer          pauses a timer
      Reset Timer         puts a timer back to its configured start (0
                          for count-up)
      Volume              continuously ties DFPlayer volume to the
                          trigger axis's position, between a configured
                          Min and Max (0-30 each) - the other continuous
                          function, alongside Override Channel. Needs an
                          axis, not a button - the row flags itself with
                          "!axis" if the learned trigger isn't one. There
                          is no Active/Inactive for this one; it just
                          always tracks the axis.

    Every function except Override Channel and Volume fires once per
    transition into its condition, not repeatedly while held - holding a
    button down doesn't replay a sound every frame.

    Deleting a Function row or a Timer always asks first, the same as
    deleting a profile: pick the item's own Delete row, then hold OK to
    actually remove it - a short OK on that confirm screen does nothing
    at all, so nothing is ever deleted by one accidental tap. BACK cancels
    immediately either way, no need to hold it.


ELRS MODEL MATCH
-----------------
Menu > Settings has a Model ID row (0-255, "off" at 0) - this is ExpressLRS's
own per-model numbering, the same thing EdgeTX sets from Model Setup >
Internal/External RF > Receiver number. The module uses it to pick which
of its own stored RF settings to use, and - if the module's own "Model
Match" toggle is separately turned on (that toggle lives in its live
parameter tree, so it's already reachable from Menu > ELRS Setup, not
here) - to refuse a receiver that isn't bound for this exact number,
preventing an accidental connection to the wrong aircraft.

TinyTx sends it to the connected module as soon as it changes, and again
every time the module is freshly detected - a module that resets forgets
what it was told and falls back to Model ID 0 until it hears otherwise,
so this matches what EdgeTX itself does to recover from that.

Model ID travels with a profile (Menu > Profiles): saving one remembers
its Model ID, and loading one re-sends it to the module immediately, the
same way the channel mix and calibration already do. That's what actually
answers "can I have a different Model ID for each of my models" - give
each one its own saved profile (a named slot works for this even outside
USB mode) and switching between them switches the Model ID too.

A note on how this was implemented: unlike the parameters in Menu > ELRS
Setup, which are fully self-describing over CRSF, Model ID doesn't appear
to be one of those - EdgeTX seems to send it as a CRSF PARAMETER_WRITE
with field id 0, inferred from ExpressLRS's own firmware source (which
keeps a dedicated variable for it, separate from its normal per-field
parameter data) and from an EdgeTX bugfix changelog describing exactly
this resend-after-reset behavior - not from a written specification,
since one doesn't seem to exist for this particular frame. It matches
multiple independent, official sources, which is why it's implemented
here, but flagging it since it's a step down in certainty from the fully
documented parts of this project.


PROFILES
--------
Menu > Profiles auto-saves and auto-loads a channel mix per USB controller,
keyed by its VID/PID - plug the same gamepad back in and its mapping comes
back with no menu interaction needed.

That auto-profile is still one-per-controller, though, so it doesn't cover
having several different setups for the same controller (or any setup at
all in a mode with no USB device to key on - SBUS, PPM, ELRS Repeater,
Pot/Btn UART). "Save as new slot" on that same menu covers that: it saves
the current channel mix and calibration as a new, independent, hand-picked
profile ("Slot 1", "Slot 2", ...) that isn't tied to any device, so it can
be loaded back regardless of what's plugged in or which Device Mode is
active. Load and Delete work the same way on either kind from the same
list - up to 16 profiles total, device-keyed and named slots sharing the
one pool.

What a profile holds: all 16 channels (mixes, expo, curve, weight, trim,
failsafe, trim values and trim-button assignments), axis calibration, the
ELRS Model ID, all Special Functions and all Timers (with their minute-call
switches), and each channel's trim Sound on/off. Everything else (device mode, output/link settings, ELRS setup,
sound setup, safety) is global. Loading a profile restarts its timers
from their start values. A profile saved by older firmware may lack Special
Functions and/or Timers; loading one leaves your current ones alone.

Quick save: holding OK and BACK together for about half a second, on any
screen, writes the current settings back into the profile you are on (the
name shown on the main screen) and pops up "Changes saved in <name>". With
no profile loaded it says so instead - use Profiles > Save first.

Holding BACK on the main Status screen jumps straight to this list - the
quickest way to swap models without going through the Main Menu first. It
lands on the list itself (past the two Save rows) whenever at least one
profile exists.

After a power cycle the profile that was active at power-off is loaded
again (from its saved copy - changes you did not save into it with OK+BACK
or Profiles > Save are not kept), and it stays in charge when the
controller connects, even if that controller has a profile of its own.
Plugging a controller in later, after boot, loads that controller's own
profile as before.

Renaming: pick a profile in the list, choose Rename. UP/DOWN change the
character under the underline, OK moves to the next one, BACK to the
previous (or out, from the first), hold OK to save, hold BACK to cancel.
Names are up to 13 characters (A-Z, 0-9, - _ . : and space). The main
screen always shows the active profile's name - in double-size letters up
to 9 characters, normal size for longer ones - or "No profile" when none
is active.

Deleting a profile asks first, and OK must be held to actually confirm it
- a short OK on that screen does nothing, so a delete can't happen from a
single accidental tap. BACK cancels immediately, no need to hold it. The
same hold-to-confirm rule applies everywhere else something gets deleted:
Function rows, Timers, and clearing a channel's whole setup.


FILES
-----
  TinyTx.ino          main sketch, USB callbacks, both core loops
  config.h           pins, constants, all persisted structures
  config_manager.h   EEPROM settings with deferred writes
  profiles.h         per-device profiles on LittleFS
  crsf.h/.cpp        CRSF output, half-duplex handling, ELRS parameters
  hid_parser.h/.cpp  HID report descriptor parser plus raw byte fallback
  input.h/.cpp       unified input model, all protocol parsers, mixer
  teach.h            channel learning detector
  xinput_host.h      Xbox 360 / One vendor class driver
  usb_diag.h         diagnostic counters
  ui.h/.cpp          menu system and OLED rendering
  splash.h           128x64 startup logo bitmap
  storage.h          filesystem mount and version-tolerant record I/O
  ppm_out.h          PPM trainer output
  sound.h            DFPlayer voice announcements

See TinyTx_Manual.pdf (formerly GamePad2ELRS_Manual.pdf) for documentation
of what every menu screen configures. It still shows the project's old name
and its button diagrams still show the original 7-button layout
(UP/DOWN/LEFT/RIGHT/PRESS/SET/RST) - ignore those and use the 4-button
scheme (UP/DOWN/OK/BACK) described above instead; the screens and settings
themselves are unchanged.


══════════════════════════════════════════════════════════════════════
TinyTxCompanion - settings & profile manager (browser)
══════════════════════════════════════════════════════════════════════
TinyTxCompanion.html is a single file: open it in Chrome or Edge (it uses
Web Serial). It edits every setting, and backs up / restores / creates
profiles. Firmware side: companion.h.

WIRING (replaces the ELRS module for the session)
    unplug the ELRS module from its connector, then
    adapter RX  ->  GP0 (board TX)
    adapter TX  ->  GP1 (board RX)
    adapter GND ->  GND
  3.3 V logic only. The adapter must do 420000 baud (FTDI FT232 and
  CP210x do; some CH340 clones don't). Board must be in CRSF output mode
  and two-pin (not "single pin") line mode.

USE
    1. Open TinyTxCompanion.html, press Connect, pick the adapter.
       (If Chrome refuses file:// pages, run  python -m http.server  in
       the folder and open http://localhost:8000/TinyTxCompanion.html)
    2. Profiles: Load / Edit / Copy / Rename / Download / Delete, New
       profile (blank, or a copy), Back up everything (one .json with all
       profiles + global settings), Restore from file.
    3. Edit tabs change a working copy; "Write to board" sends only what
       changed and saves it. Mode / output / line changes need a restart.
    4. Live channels shows the 16 outputs in real time.
  While connected the board sends no CRSF. The session ends on
  Disconnect, after 12 s of silence, or on restart; then normal output
  resumes. With the ELRS module connected nothing changes.
