.. SPDX-License-Identifier: GPL-2.0

==========================
PCM data buffer descriptor
==========================

A sound server that lets a client write into the mmap buffer of a PCM
stream has to give that client a file descriptor to map. A duplicate of
the PCM descriptor also gives the client every ioctl of the stream, and
it refers to the device node, which a security policy may not allow the
client to use.

``SNDRV_PCM_IOCTL_ANONYMOUS_DUP`` returns a new descriptor for the stream
of the PCM descriptor it is called on. The argument points to an ``int``
holding the mode, and the only mode is ``SNDRV_PCM_ANON_DUP_BUFFER``::

    int mode = SNDRV_PCM_ANON_DUP_BUFFER;
    int fd = ioctl(pcm_fd, SNDRV_PCM_IOCTL_ANONYMOUS_DUP, &mode);

The ioctl is available from PCM protocol version 2.0.19.

The returned descriptor is close-on-exec and has the access mode of the
PCM descriptor. It supports ``mmap()`` of the data buffer, with the
offsets and limits of the PCM descriptor, and ``close()``. It has no
ioctls, no read or write, and it does not map the status and control
records. The stream position stays with the holder of the PCM descriptor.

The descriptor is a file named ``[snd-pcm-buffer]`` on the shared anonymous
inode. A security module checks only that a client may use descriptors of
the process that passed it, as it does for a dma-buf, and the client needs
no access to the sound device nodes.

The descriptor and its mappings are references to the stream, like the
descriptors of an ``O_APPEND`` open. The buffer can not be freed or
reallocated while it is mapped, and the substream stays open until the
last descriptor is closed and the last mapping is removed. A server that
closes its PCM descriptor first leaves the device busy until the client
lets go of the buffer.
