// SPDX-License-Identifier: GPL-2.0
/*
 * SNDRV_PCM_IOCTL_ANONYMOUS_DUP on the first playback PCM that accepts
 * mmap access.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sound/asound.h>

#include "kselftest_harness.h"

static int configure(int pcm)
{
	struct snd_pcm_hw_params params = { .rmask = ~0U, .info = ~0U };
	struct snd_mask *access = &params.masks[SNDRV_PCM_HW_PARAM_ACCESS -
						SNDRV_PCM_HW_PARAM_FIRST_MASK];
	unsigned int i;

	memset(params.masks, 0xff, sizeof(params.masks));

	for (i = 0; i < ARRAY_SIZE(params.intervals); i++)
		params.intervals[i].max = UINT_MAX;

	memset(access, 0, sizeof(*access));
	access->bits[0] = 1 << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;

	return ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &params);
}

static int open_pcm(char *path, size_t size)
{
	int card, device, pcm;

	for (card = 0; card < 32; card++) {
		for (device = 0; device < 32; device++) {
			snprintf(path, size, "/dev/snd/pcmC%dD%dp", card,
				 device);

			pcm = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
			if (pcm < 0)
				continue;

			if (!configure(pcm))
				return pcm;

			close(pcm);
		}
	}

	return -1;
}

static int duplicate(int pcm, int mode)
{
	return ioctl(pcm, SNDRV_PCM_IOCTL_ANONYMOUS_DUP, &mode);
}

/* Substreams of @path that are open, not counting the probing open. */
static int busy_substreams(const char *path, int count)
{
	struct snd_pcm_info info;
	int pcm, err;

	pcm = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (pcm < 0)
		return errno == EBUSY ? count : -1;

	err = ioctl(pcm, SNDRV_PCM_IOCTL_INFO, &info);
	close(pcm);

	return err ? -1 : count - info.subdevices_avail - 1;
}

FIXTURE(anon_dup) {
	char path[64];
	size_t page;
	int pcm;
	int buffer;
};

FIXTURE_SETUP(anon_dup)
{
	self->page = sysconf(_SC_PAGESIZE);
	self->buffer = -1;

	self->pcm = open_pcm(self->path, sizeof(self->path));
	if (self->pcm < 0)
		SKIP(return, "No playback PCM with mmap access");

	self->buffer = duplicate(self->pcm, SNDRV_PCM_ANON_DUP_BUFFER);
	ASSERT_LE(0, self->buffer);
}

FIXTURE_TEARDOWN(anon_dup)
{
	if (self->buffer >= 0)
		close(self->buffer);

	if (self->pcm >= 0)
		close(self->pcm);
}

TEST_F(anon_dup, rejects_other_modes)
{
	ASSERT_EQ(-1, duplicate(self->pcm, 0));
	ASSERT_EQ(EINVAL, errno);

	ASSERT_EQ(-1, duplicate(self->pcm, SNDRV_PCM_ANON_DUP_BUFFER + 1));
	ASSERT_EQ(EINVAL, errno);
}

TEST_F(anon_dup, is_a_named_anonymous_inode)
{
	char link[64], name[64] = "";

	snprintf(link, sizeof(link), "/proc/self/fd/%d", self->buffer);
	ASSERT_LT(0, readlink(link, name, sizeof(name) - 1));
	ASSERT_STREQ("anon_inode:[snd-pcm-buffer]", name);

	ASSERT_EQ(FD_CLOEXEC, fcntl(self->buffer, F_GETFD));
}

TEST_F(anon_dup, exposes_only_the_data_buffer)
{
	char byte = 0;
	int version;

	ASSERT_EQ(-1, ioctl(self->buffer, SNDRV_PCM_IOCTL_PVERSION, &version));
	ASSERT_EQ(ENOTTY, errno);

	ASSERT_EQ(-1, write(self->buffer, &byte, 1));
	ASSERT_EQ(EINVAL, errno);

	ASSERT_EQ(-1, read(self->buffer, &byte, 1));
	ASSERT_EQ(EINVAL, errno);

	ASSERT_EQ(MAP_FAILED, mmap(NULL, self->page, PROT_READ, MAP_SHARED,
				   self->buffer,
				   SNDRV_PCM_MMAP_OFFSET_STATUS));
	ASSERT_EQ(MAP_FAILED, mmap(NULL, self->page, PROT_READ, MAP_SHARED,
				   self->buffer,
				   SNDRV_PCM_MMAP_OFFSET_CONTROL));
}

TEST_F(anon_dup, maps_the_buffer_of_the_stream)
{
	char *stream, *buffer;

	stream = mmap(NULL, self->page, PROT_READ | PROT_WRITE, MAP_SHARED,
		      self->pcm, 0);
	ASSERT_NE(MAP_FAILED, stream);

	buffer = mmap(NULL, self->page, PROT_READ | PROT_WRITE, MAP_SHARED,
		      self->buffer, 0);
	ASSERT_NE(MAP_FAILED, buffer);

	memset(buffer, 0x5a, self->page);
	ASSERT_EQ(0x5a, stream[0]);
	ASSERT_EQ(0x5a, stream[self->page - 1]);

	memset(stream, 0x3c, self->page);
	ASSERT_EQ(0x3c, buffer[0]);
	ASSERT_EQ(0x3c, buffer[self->page - 1]);

	ASSERT_EQ(0, munmap(buffer, self->page));
	ASSERT_EQ(0, munmap(stream, self->page));
}

TEST_F(anon_dup, keeps_a_mapped_buffer_allocated)
{
	void *buffer;

	buffer = mmap(NULL, self->page, PROT_READ | PROT_WRITE, MAP_SHARED,
		      self->buffer, 0);
	ASSERT_NE(MAP_FAILED, buffer);

	ASSERT_EQ(-1, ioctl(self->pcm, SNDRV_PCM_IOCTL_HW_FREE));
	ASSERT_EQ(EBADFD, errno);
	ASSERT_EQ(-1, configure(self->pcm));
	ASSERT_EQ(EBADFD, errno);

	ASSERT_EQ(0, munmap(buffer, self->page));
	ASSERT_EQ(0, ioctl(self->pcm, SNDRV_PCM_IOCTL_HW_FREE));

	ASSERT_EQ(MAP_FAILED, mmap(NULL, self->page, PROT_READ | PROT_WRITE,
				   MAP_SHARED, self->buffer, 0));
	ASSERT_EQ(EBADFD, errno);
}

TEST_F(anon_dup, keeps_the_stream_open)
{
	struct snd_pcm_info info;
	void *buffer;
	int count;

	ASSERT_EQ(0, ioctl(self->pcm, SNDRV_PCM_IOCTL_INFO, &info));
	count = info.subdevices_count;
	ASSERT_EQ(count - 1, info.subdevices_avail);

	buffer = mmap(NULL, self->page, PROT_READ | PROT_WRITE, MAP_SHARED,
		      self->buffer, 0);
	ASSERT_NE(MAP_FAILED, buffer);

	ASSERT_EQ(0, close(self->pcm));
	self->pcm = -1;
	ASSERT_EQ(1, busy_substreams(self->path, count));

	ASSERT_EQ(0, close(self->buffer));
	self->buffer = -1;
	ASSERT_EQ(1, busy_substreams(self->path, count));

	memset(buffer, 0, self->page);
	ASSERT_EQ(0, munmap(buffer, self->page));
	ASSERT_EQ(0, busy_substreams(self->path, count));
}

TEST_HARNESS_MAIN
