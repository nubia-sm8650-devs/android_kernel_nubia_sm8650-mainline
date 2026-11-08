.. SPDX-License-Identifier: GPL-2.0-only

AudioReach runtime controls
==========================

Dynamic module registration
---------------------------

The ``AudioReach Module Registration`` and
``AudioReach Module Deregistration`` byte controls accept native AMDB
REGISTER and DEREGISTER payloads through ALSA TLV writes. The standard
ALSA byte-control TLV header is not part of the AMDB payload.

Submit registration before opening graphs that use the modules. Each write
waits for the DSP command response. The controls do not retain registration
data or expose explicit AMDB LOAD and UNLOAD operations.

Module events
-------------

Each PCM exposes a ``Playback Graph Events`` or ``Capture Graph Events``
control with the PCM device number and ``SNDRV_CTL_ELEM_IFACE_PCM``.
Open and configure the PCM before registering events on its processing or
device graphs. Registration is also available for modules in the PCM's
own graph before hardware parameters are installed.

Use ``SNDRV_CTL_IOCTL_TLV_WRITE`` with the following native-endian words
and the event's optional configuration data.

.. code-block:: text

   APM_CMD_REGISTER_MODULE_EVENTS (0x0100100e)
   length of the registration record and configuration, in bytes
   module instance ID
   event ID
   is_register (one to register, zero to unregister)
   error_code (zero)
   configuration size, in bytes
   reserved (zero)
   configuration data

One write submits one registration record. The target must belong to the
PCM's own graph or a graph acquired by that PCM. VCPM service events are
restricted to voice-call graphs.

Subscribe to ALSA control events, then drain the control with
``SNDRV_CTL_IOCTL_TLV_READ``. Each successful read returns one record.

.. code-block:: text

   APM_EVENT_MODULE_TO_CLIENT (0x03001000)
   length of the following words and payload, in bytes
   source module instance ID
   event ID
   event payload size, in bytes
   event payload

An empty queue returns ``-EAGAIN``. A short buffer returns ``-ENOSPC``
without consuming the record. Queue loss is reported as ``-EOVERFLOW``;
subsequent reads can drain retained records. Closing the PCM discards its
queue. Unregister events before detaching their graphs, and register them
again after reopening those graphs.

DSP restart
-----------

GPR service removal disconnects the ALSA card. Userspace must handle the
disconnected control and PCM descriptors, close them, wait for the card to
return and reopen it. Replay module registration before restoring graphs,
calibration and event subscriptions. Registration and event state are not
cached across card recreation.
