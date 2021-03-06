/* Copyright (c) 2012 Dave Reisner
 *
 * ponymix.c
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <err.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/pulseaudio.h>

#define UNUSED __attribute__((unused))

#define CLAMP(x, low, high) \
	__extension__ ({ \
		typeof(x) _x = (x); \
		typeof(low) _low = (low); \
		typeof(high) _high = (high); \
		((_x > _high) ? _high : ((_x < _low) ? _low : _x)); \
	})

enum connectstate {
	STATE_CONNECTING = 0,
	STATE_CONNECTED,
	STATE_ERROR
};

enum mode {
	MODE_DEVICE = 0,
	MODE_APP,
	MODE_INVALID
};

enum action {
	ACTION_DEFAULTS = 0,
	ACTION_LIST,
	ACTION_GETVOL,
	ACTION_SETVOL,
	ACTION_GETBAL,
	ACTION_SETBAL,
	ACTION_ADJBAL,
	ACTION_INCREASE,
	ACTION_DECREASE,
	ACTION_MUTE,
	ACTION_UNMUTE,
	ACTION_TOGGLE,
	ACTION_ISMUTED,
	ACTION_SETDEFAULT,
	ACTION_MOVE,
	ACTION_KILL,
	ACTION_INVALID
};

static const char *actions[ACTION_INVALID] = {
	[ACTION_DEFAULTS] = "defaults",
	[ACTION_LIST] = "list",
	[ACTION_GETVOL] = "get-volume",
	[ACTION_SETVOL] = "set-volume",
	[ACTION_GETBAL] = "get-balance",
	[ACTION_SETBAL] = "set-balance",
	[ACTION_ADJBAL] = "adj-balance",
	[ACTION_INCREASE] = "increase",
	[ACTION_DECREASE] = "decrease",
	[ACTION_MUTE] = "mute",
	[ACTION_UNMUTE] = "unmute",
	[ACTION_TOGGLE] = "toggle",
	[ACTION_ISMUTED] = "is-muted",
	[ACTION_SETDEFAULT] = "set-default",
	[ACTION_MOVE] = "move",
	[ACTION_KILL] = "kill"
};

struct io_t {
	uint32_t idx;
	char *name;
	char *desc;
	const char *pp_name;
	pa_cvolume volume;
	pa_channel_map channels;
	int volume_percent;
	int balance;
	int mute;

	struct ops_t {
		pa_operation *(*mute)(pa_context *, uint32_t, int, pa_context_success_cb_t, void *);
		pa_operation *(*setvol)(pa_context *, uint32_t, const pa_cvolume *, pa_context_success_cb_t, void *);
		pa_operation *(*setdefault)(pa_context *, const char *, pa_context_success_cb_t, void *);
		pa_operation *(*move)(pa_context *, uint32_t, uint32_t, pa_context_success_cb_t, void *);
		pa_operation *(*kill)(pa_context *, uint32_t, pa_context_success_cb_t, void *);
	} op;

	struct io_t *next;
};

struct pulseaudio_t {
	pa_context *cxt;
	pa_mainloop *mainloop;
	enum connectstate state;
	int success;

	struct io_t *head;
};

static int xstrtol(const char *str, long *out)
{
	char *end = NULL;

	if (str == NULL || *str == '\0')
		return -1;
	errno = 0;

	*out = strtol(str, &end, 10);
	if (errno || str == end || (end && *end))
		return -1;

	return 0;
}

static void populate_levels(struct io_t *node)
{
	node->volume_percent = (int)(((double)pa_cvolume_avg(&node->volume) * 100)
			/ PA_VOLUME_NORM);
	node->balance = (int)((double)pa_cvolume_get_balance(&node->volume,
			&node->channels) * 100);
}

#define IO_NEW(io, info, pp) \
	io = calloc(1, sizeof(struct io_t)); \
	io->idx = info->index; \
	io->mute = info->mute; \
	io->name = strdup(info->name); \
	io->pp_name = pp; \
	memcpy(&io->volume, &info->volume, sizeof(pa_cvolume)); \
	memcpy(&io->channels, &info->channel_map, sizeof(pa_channel_map)); \
	populate_levels(io);

static struct io_t *sink_new(const pa_sink_info *info)
{
	struct io_t *sink;

	IO_NEW(sink, info, "sink");
	sink->desc = strdup(info->description);
	sink->op.mute = pa_context_set_sink_mute_by_index;
	sink->op.setvol = pa_context_set_sink_volume_by_index;
	sink->op.setdefault = pa_context_set_default_sink;

	return sink;
}

static struct io_t *sink_input_new(const pa_sink_input_info *info)
{
	struct io_t *sink;

	IO_NEW(sink, info, "sink");
	sink->desc = strdup(
		pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME));
	sink->op.mute = pa_context_set_sink_input_mute;
	sink->op.setvol = pa_context_set_sink_input_volume;
	sink->op.move = pa_context_move_sink_input_by_index;
	sink->op.kill = pa_context_kill_sink_input;

	return sink;
}

static struct io_t *source_new(const pa_source_info *info)
{
	struct io_t *source;

	IO_NEW(source, info, "source");
	source->desc = strdup(info->description);
	source->op.mute = pa_context_set_source_mute_by_index;
	source->op.setvol = pa_context_set_source_volume_by_index;
	source->op.setdefault = pa_context_set_default_source;

	return source;
}

static struct io_t *source_output_new(const pa_source_output_info *info)
{
	struct io_t *source;

	IO_NEW(source, info, "source");
	source->desc = strdup(
		pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME));
	source->op.mute = pa_context_set_source_output_mute;
	source->op.setvol = pa_context_set_source_output_volume;
	source->op.move = pa_context_move_source_output_by_index;
	source->op.kill = pa_context_kill_source_output;

	return source;
}

static void sink_add_cb(pa_context UNUSED *c, const pa_sink_info *i, int eol,
		void *raw)
{
	struct pulseaudio_t *pulse = raw;
	struct io_t *sink;

	if (eol)
		return;

	sink = sink_new(i);
	sink->next = pulse->head;
	pulse->head = sink;
}

static void sink_input_add_cb(pa_context UNUSED *c, const pa_sink_input_info *i, int eol,
		void *raw)
{
	struct pulseaudio_t *pulse = raw;
	struct io_t *sink;

	if (eol)
		return;

	sink = sink_input_new(i);
	sink->next = pulse->head;
	pulse->head = sink;
}

static void source_add_cb(pa_context UNUSED *c, const pa_source_info *i, int eol, void *raw)
{
	struct pulseaudio_t *pulse = raw;
	struct io_t *source;

	if (eol)
		return;

	source = source_new(i);
	source->next = pulse->head;
	pulse->head = source;
}

static void source_output_add_cb(pa_context UNUSED *c, const pa_source_output_info *i, int eol,
		void *raw)
{
	struct pulseaudio_t *pulse = raw;
	struct io_t *source;

	if (eol)
		return;

	source = source_output_new(i);
	source->next = pulse->head;
	pulse->head = source;
}

static void server_info_cb(pa_context UNUSED *c, const pa_server_info *i,
		void *raw)
{
	const char **sink_name = raw;

	*sink_name = i->default_sink_name;
}

static void source_info_cb(pa_context UNUSED *c, const pa_server_info *i, void *raw)
{
	const char **source_name = raw;

	*source_name = i->default_source_name;
}

static void state_cb(pa_context UNUSED *c, void *raw)
{
	struct pulseaudio_t *pulse = raw;

	switch (pa_context_get_state(pulse->cxt)) {
	case PA_CONTEXT_READY:
		pulse->state = STATE_CONNECTED;
		break;
	case PA_CONTEXT_FAILED:
		pulse->state = STATE_ERROR;
		break;
	case PA_CONTEXT_UNCONNECTED:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_TERMINATED:
		break;
	}
}

static void success_cb(pa_context UNUSED *c, int success, void *raw)
{
	struct pulseaudio_t *pulse = raw;
	pulse->success = success;
}

static void pulse_async_wait(struct pulseaudio_t *pulse, pa_operation *op)
{
	while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
		pa_mainloop_iterate(pulse->mainloop, 1, NULL);
}

static int set_volume(struct pulseaudio_t *pulse, struct io_t *dev, long v)
{
	pa_cvolume *vol = pa_cvolume_set(&dev->volume, dev->volume.channels,
			(int)fmax((double)(v + .5) * PA_VOLUME_NORM / 100, 0));
	pa_operation *op = pulse->head->op.setvol(pulse->cxt, dev->idx, vol,
			success_cb, pulse);
	pulse_async_wait(pulse, op);

	if (pulse->success)
		printf("%ld\n", v);
	else {
		int err = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to set volume: %s\n", pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static int set_balance(struct pulseaudio_t *pulse, struct io_t *dev, long v)
{
	pa_cvolume *vol;
	pa_operation *op;

	if (pa_channel_map_valid(&dev->channels) == 0) {
		warnx("can't set balance on that device.");
		return 1;
	}

	vol = pa_cvolume_set_balance(&dev->volume, &dev->channels, v / 100.0);
	op = dev->op.setvol(pulse->cxt, dev->idx, vol, success_cb, pulse);
	pulse_async_wait(pulse, op);

	if (pulse->success)
		printf("%ld\n", v);
	else {
		int err = pa_context_errno(pulse->cxt);
		warnx("failed to set balance: %s", pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static int set_mute(struct pulseaudio_t *pulse, struct io_t *dev, int mute)
{
	pa_operation* op;

	/* new effective volume */
	printf("%d\n", mute ? 0 : dev->volume_percent);

	op = pulse->head->op.mute(pulse->cxt, dev->idx, mute,
			success_cb, pulse);

	pulse_async_wait(pulse, op);

	if (!pulse->success) {
		int err = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to mute device: %s\n", pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static int kill_client(struct pulseaudio_t *pulse, struct io_t *dev)
{
	pa_operation *op;

	if (dev->op.kill == NULL) {
		warnx("only clients can be killed");
		return 1;
	}

	op = dev->op.kill(pulse->cxt, dev->idx, success_cb, pulse);
	pulse_async_wait(pulse, op);

	if (!pulse->success) {
		int err = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to kill client: %s\n", pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static int move_client(struct pulseaudio_t *pulse, struct io_t *dev)
{
	pa_operation* op;

	if (dev->next == NULL) {
		warnx("no destination to move to");
		return 1;
	}
	if (dev->next->op.move == NULL) {
		warnx("only clients can be moved");
		return 1;
	}

	op = dev->next->op.move(pulse->cxt, dev->next->idx, dev->idx, success_cb,
			pulse);

	pulse_async_wait(pulse, op);

	if (!pulse->success) {
		int err = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to move client: %s\n", pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static void print(struct io_t *dev)
{
	printf("%s %2d: %s\n  %s\n  Avg. Volume: %d%% %s\n", dev->pp_name,
			dev->idx, dev->name, dev->desc, dev->volume_percent,
			dev->mute ? "[Muted]" : "");
}

static void print_all(struct pulseaudio_t *pulse)
{
	struct io_t *dev = pulse->head;

	while (dev) {
		print(dev);
		dev = dev->next;
	}
}

static void populate_sinks(struct pulseaudio_t *pulse, enum mode mode)
{
	pa_operation *op;

	switch (mode) {
	case MODE_APP:
		op = pa_context_get_sink_input_info_list(pulse->cxt, sink_input_add_cb, pulse);
		break;
	case MODE_DEVICE:
	default:
		op = pa_context_get_sink_info_list(pulse->cxt, sink_add_cb, pulse);
	}

	pulse_async_wait(pulse, op);
	pa_operation_unref(op);
}

static int get_sink_by_name(struct pulseaudio_t *pulse, const char *name, enum mode mode)
{
	pa_operation *op;

	switch (mode) {
	case MODE_APP:
	{
		long id;
		if (xstrtol(name, &id) < 0) {
			warnx("application sink not valid id: %s", name);
			return 1;
		}
		op = pa_context_get_sink_input_info(pulse->cxt, (uint32_t)id, sink_input_add_cb, pulse);
		break;
	}
	case MODE_DEVICE:
	default:
		op = pa_context_get_sink_info_by_name(pulse->cxt, name, sink_add_cb, pulse);
	}

	pulse_async_wait(pulse, op);
	pa_operation_unref(op);

	return 0;
}

static int get_default_sink(struct pulseaudio_t *pulse)
{
	const char *sink_name;
	pa_operation *op = pa_context_get_server_info(pulse->cxt, server_info_cb,
			&sink_name);
	pulse_async_wait(pulse, op);
	pa_operation_unref(op);

	return get_sink_by_name(pulse, sink_name, MODE_DEVICE);
}

static void populate_sources(struct pulseaudio_t *pulse, enum mode mode)
{
	pa_operation *op;

	switch (mode) {
	case MODE_APP:
		op = pa_context_get_source_output_info_list(pulse->cxt, source_output_add_cb, pulse);
		break;
	case MODE_DEVICE:
	default:
		op = pa_context_get_source_info_list(pulse->cxt, source_add_cb, pulse);
	}

	pulse_async_wait(pulse, op);
	pa_operation_unref(op);
}

static int get_source_by_name(struct pulseaudio_t *pulse, const char *name, enum mode mode)
{
	pa_operation *op;

	switch (mode) {
	case MODE_APP:
	{
		long id;
		if (xstrtol(name, &id) < 0) {
			warnx("application source not valid id: %s", name);
			return 1;
		}
		op = pa_context_get_source_output_info(pulse->cxt, (uint32_t)id, source_output_add_cb, pulse);
		break;
	}
	case MODE_DEVICE:
	default:
		op = pa_context_get_source_info_by_name(pulse->cxt, name, source_add_cb, pulse);
	}

	pulse_async_wait(pulse, op);
	pa_operation_unref(op);

	return 0;
}

static int get_default_source(struct pulseaudio_t *pulse)
{
	const char *source_name;
	pa_operation *op = pa_context_get_server_info(pulse->cxt, source_info_cb,
			&source_name);
	pulse_async_wait(pulse, op);
	pa_operation_unref(op);

	return get_source_by_name(pulse, source_name, MODE_DEVICE);
}

static int set_default(struct pulseaudio_t *pulse, struct io_t *dev)
{
	pa_operation *op;

	if (dev->op.setdefault == NULL) {
		warnx("valid operation only for devices");
		return 1;
	}

	op = dev->op.setdefault(pulse->cxt, dev->name, success_cb, pulse);
	pulse_async_wait(pulse, op);

	if (!pulse->success) {
		int err = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to set default %s to %s: %s\n", dev->pp_name,
				dev->name, pa_strerror(err));
	}

	pa_operation_unref(op);

	return !pulse->success;
}

static int pulse_connect(struct pulseaudio_t *pulse)
{
	pa_context_connect(pulse->cxt, NULL, PA_CONTEXT_NOFLAGS, NULL);
	while (pulse->state == STATE_CONNECTING)
		pa_mainloop_iterate(pulse->mainloop, 1, NULL);

	if (pulse->state == STATE_ERROR) {
		int r = pa_context_errno(pulse->cxt);
		fprintf(stderr, "failed to connect to pulse daemon: %s\n",
				pa_strerror(r));
		return 1;
	}

	return 0;
}

static int pulse_init(struct pulseaudio_t *pulse)
{
	pulse->mainloop = pa_mainloop_new();
	pulse->cxt = pa_context_new(pa_mainloop_get_api(pulse->mainloop), "bestpony");
	pulse->state = STATE_CONNECTING;
	pulse->head = NULL;

	pa_context_set_state_callback(pulse->cxt, state_cb, pulse);
	return pulse_connect(pulse);
}

static void pulse_deinit(struct pulseaudio_t *pulse)
{
	struct io_t *node = pulse->head;

	pa_context_disconnect(pulse->cxt);
	pa_mainloop_free(pulse->mainloop);

	while (node) {
		node = pulse->head->next;
		free(pulse->head->name);
		free(pulse->head->desc);
		free(pulse->head);
		pulse->head = node;
	}
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fprintf(out, "usage: %s [options] <command>...\n", program_invocation_short_name);
	fputs("\nOptions:\n"
	      " -h, --help              display this help and exit\n"

	      "\n -d, --device            set device mode (default)\n"
	      " -a, --app               set application mode\n"

	      "\n -o, --sink=<name>       control a sink other than the default\n"
	      " -i, --source=<name>     control a source\n", out);

	fputs("\nCommon Commands:\n"
	      "  list                   list available devices\n"
	      "  get-volume             get volume for device\n"
	      "  set-volume VALUE       set volume for device\n"
	      "  get-balance            get balance for device\n"
	      "  set-balance VALUE      set balance for device\n"
	      "  adj-balance VALUE      increase or decrease balance for device\n"
	      "  increase VALUE         increase volume\n", out);
	fputs("  decrease VALUE         decrease volume\n"
	      "  mute                   mute device\n"
	      "  unmute                 unmute device\n"
	      "  toggle                 toggle mute\n"
	      "  is-muted               check if muted\n", out);

	fputs("\nDevice Commands:\n"
	      "  defaults               list default devices\n"
	      "  set-default DEVICE_ID  set default device by ID\n"

	      "\nApplication Commands:\n"
	      "  move APP_ID DEVICE_ID  move application stream by ID to device ID\n"
	      "  kill APP_ID            kill an application stream by ID\n", out);

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static enum action string_to_verb(const char *string)
{
	size_t i;

	for (i = 0; i < ACTION_INVALID; i++)
		if (strcmp(actions[i], string) == 0)
			break;

	return i;
}

static int do_verb(struct pulseaudio_t *pulse, enum action verb, int value)
{
	switch (verb) {
	case ACTION_GETVOL:
		printf("%d\n", pulse->head->volume_percent);
		return 0;
	case ACTION_SETVOL:
		return set_volume(pulse, pulse->head, CLAMP(value, 0, 150));
	case ACTION_GETBAL:
		printf("%d\n", pulse->head->balance);
		return 0;
	case ACTION_SETBAL:
		return set_balance(pulse, pulse->head, CLAMP(value, -100, 100));
	case ACTION_ADJBAL:
		return set_balance(pulse, pulse->head,
				CLAMP(pulse->head->balance + value, -100, 100));
	case ACTION_INCREASE:
		return set_volume(pulse, pulse->head,
				CLAMP(pulse->head->volume_percent + value, 0, 100));
	case ACTION_DECREASE:
		return set_volume(pulse, pulse->head,
				CLAMP(pulse->head->volume_percent - value, 0, 100));
	case ACTION_MUTE:
		return set_mute(pulse, pulse->head, 1);
	case ACTION_UNMUTE:
		return set_mute(pulse, pulse->head, 0);
	case ACTION_TOGGLE:
		return set_mute(pulse, pulse->head, !pulse->head->mute);
	case ACTION_ISMUTED:
		return !pulse->head->mute;
	case ACTION_MOVE:
		return move_client(pulse, pulse->head);
	case ACTION_KILL:
		return kill_client(pulse, pulse->head);
	case ACTION_SETDEFAULT:
		return set_default(pulse, pulse->head);
	default:
		errx(EXIT_FAILURE, "internal error: unhandled verb id %d\n", verb);
	}
}

int main(int argc, char *argv[])
{
	struct pulseaudio_t pulse;
	enum action verb;
	char *id = NULL, *arg = NULL;
	long value = 0;
	enum mode mode = MODE_DEVICE;
	int rc = EXIT_SUCCESS;

	const char *pp_name = "sink";
	int (*fn_get_default)(struct pulseaudio_t *) = get_default_sink;
	int (*fn_get_by_name)(struct pulseaudio_t *, const char *, enum mode) = get_sink_by_name;

	static const struct option opts[] = {
		{ "app", no_argument, 0, 'a' },
		{ "device", no_argument, 0, 'd' },
		{ "help", no_argument, 0, 'h' },
		{ "sink", optional_argument, 0, 'o' },
		{ "source", optional_argument, 0, 'i' },
		{ 0, 0, 0, 0 },
	};

	for (;;) {
		int opt = getopt_long(argc, argv, "adhi::o::", opts, NULL);
		if (opt == -1)
			break;

		switch (opt) {
		case 'h':
			usage(stdout);
		case 'd':
			mode = MODE_DEVICE;
			break;
		case 'a':
			mode = MODE_APP;
			break;
		case 'o':
			id = optarg;
			fn_get_default = get_default_sink;
			fn_get_by_name = get_sink_by_name;
			pp_name = "sink";
			break;
		case 'i':
			id = optarg;
			fn_get_default = get_default_source;
			fn_get_by_name = get_source_by_name;
			pp_name = "source";
			break;
		default:
			return EXIT_FAILURE;
		}
	}

	/* string -> enum */
	verb = (optind == argc) ? (mode ? ACTION_LIST : ACTION_DEFAULTS) : string_to_verb(argv[optind]);
	if (verb == ACTION_INVALID)
		errx(EXIT_FAILURE, "unknown action: %s", argv[optind]);

	optind++;
	switch (verb) {
	case ACTION_SETVOL:
	case ACTION_SETBAL:
	case ACTION_ADJBAL:
	case ACTION_INCREASE:
	case ACTION_DECREASE:
		if (optind == argc)
			errx(EXIT_FAILURE, "missing value for action '%s'", argv[optind - 1]);
		else {
			/* validate to number */
			if (xstrtol(argv[optind], &value) < 0)
				errx(EXIT_FAILURE, "invalid number: %s", argv[optind]);
		}
		break;
	case ACTION_SETDEFAULT:
	case ACTION_KILL:
		if (optind == argc)
			errx(EXIT_FAILURE, "missing arguments for action '%s'", argv[optind - 1]);
		else
			id = argv[optind];
		break;
	case ACTION_MOVE:
		if (optind > argc - 2)
			errx(EXIT_FAILURE, "missing arguments for action '%s'", argv[optind - 1]);
		else {
			id = argv[optind++];
			arg = argv[optind];
		}
		break;
	default:
		break;
	}

	/* initialize connection */
	if (pulse_init(&pulse) != 0)
		return EXIT_FAILURE;

	switch (verb) {
	case ACTION_DEFAULTS:
		get_default_sink(&pulse);
		get_default_source(&pulse);
		print_all(&pulse);
		goto done;
	case ACTION_LIST:
		populate_sinks(&pulse, mode);
		populate_sources(&pulse, mode);
		print_all(&pulse);
		goto done;
	default:
		break;
	}

	if (id && fn_get_by_name) {
		if (fn_get_by_name(&pulse, id, mode) != 0)
			goto done;
	} else if (!mode && verb != ACTION_SETDEFAULT && fn_get_default) {
		if (fn_get_default(&pulse) != 0)
			goto done;
	}

	if (pulse.head == NULL) {
		if (mode && !id)
			warnx("%s id not set, no default operations", pp_name);
		else
			warnx("%s not found: %s", pp_name, id ? id : "default");
		rc = EXIT_FAILURE;
		goto done;
	}

	if (arg && fn_get_by_name)
		fn_get_by_name(&pulse, arg, MODE_DEVICE);

	rc = do_verb(&pulse, verb, value);

done:
	/* shut down */
	pulse_deinit(&pulse);

	return rc;
}

/* vim: set noet ts=2 sw=2: */
