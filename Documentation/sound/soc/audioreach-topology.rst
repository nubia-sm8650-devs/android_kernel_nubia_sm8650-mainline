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

Cross-graph reference controls
------------------------------

Use standard non-DAPM boolean mixer controls for selection-dependent links.
For data links, supply one word array per connection with the source
instance, source output port, destination instance and destination input
port tokens. For control links, attach an ``audioreach_control_links``
block instead. A control must not mix the two declaration types.

Enabling a reference does not open either endpoint graph. If either graph
is absent, the reference is staged. Its links enter GRAPH_OPEN when the
second graph opens, or when both endpoints belong to the graph being
opened. With both graphs open, the boolean sends a link-only OPEN or CLOSE.
Control-link OPEN preserves native properties; CLOSE supplies peer
identities with no properties. A failed command retains the prior boolean
state. Prepare, start and stop remain owned by the existing graph users.

Voice subgraph tags
-------------------

Attach one ``audioreach_voice_tags`` block to a module widget in each
subgraph that supplies VCPM tag information. Set its type to
``SND_SOC_AR_TPLG_VOICE_TAG_TYPE``. The little-endian header contains the
block size, type and tag count, followed by ordered tag-ID/module-instance
pairs. An empty list remains an explicit empty declaration.

Tag IDs must be nonzero and unique within the list. Every referenced
module must belong to the owning widget's subgraph. The kernel includes
the tags in VCPM voice configuration inside GRAPH_OPEN, not a separate
module SET command.

Logger configuration
--------------------

Logger modules can be declared without logging fields when userspace
supplies their calibration. Media-format preparation leaves those modules
unchanged. Supplying legacy logging tokens selects the existing automatic
enable and logger-configuration sequence, including explicit zero values.
