.. SPDX-License-Identifier: GPL-2.0-only

AudioReach topology declarations
===============================

Module data connections
-----------------------

The module word array supports the original indexed connection tokens.
Additional connections can follow it as separate word arrays. Each array
contains ``AR_TKN_U32_MODULE_SRC_OP_PORT_ID``,
``AR_TKN_U32_MODULE_DST_IN_PORT_ID`` and
``AR_TKN_U32_MODULE_DST_INSTANCE_ID``, in that order. The source instance
is the widget's module instance. Port IDs are native DSP identities, not
indices into the declared port capacity.

Module control links
--------------------

Attach an ``audioreach_control_links`` block to the first peer's module
widget using a topology data section. Set its type to
``SND_SOC_AR_TPLG_CONTROL_LINK_TYPE``. The size includes the header and
all link records. Header words and record fields are little-endian.

Each record contains the first peer's module instance and control-port
IDs, the second peer's instance and control-port IDs, and a property
count. Each property contains its native ID, payload size and payload.
Retain native intent-list and heap properties in these records.

The kernel includes the declarations in GRAPH_OPEN. They are not module
SET data and do not establish a separate widget lifecycle or an ALSA
enable control. Userspace must select the endpoint graphs together.
