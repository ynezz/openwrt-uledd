#include <libubox/avl.h>
#include <libubox/avl-cmp.h>
#include <libubox/blobmsg.h>

#include "blob_led.h"
#include "led.h"
#include "log.h"
#include "scene.h"
#include "timer.h"

enum scene_state {
	SCENE_IDLE = 0,
	SCENE_PAUSED,
	SCENE_RUNNING,
};

struct scene_led {
	struct avl_node avl;
	struct blob_led *led;
};

struct scene {
	char *name;
	int timeout;
	int priority;
	struct avl_node avl;
	struct avl_tree leds;
	struct led_timer timer;
	enum scene_state state;
	struct scene *paused_by;
};

static struct avl_tree scene_tree = AVL_TREE_INIT(scene_tree, avl_strcmp, false, NULL);

static inline bool
is_running(struct scene *s)
{
	return s->state == SCENE_RUNNING;
}

static inline bool
is_paused(struct scene *s)
{
	return s->state == SCENE_PAUSED;
}

static struct scene_led *
scene_led_from_path(struct scene *s, const char *path)
{
	struct scene_led *led;
	return avl_find_element(&s->leds, path, led, avl);
}

static bool
has_shared_leds(struct scene *s1, struct scene *s2)
{
	struct scene_led *item;

	avl_for_each_element(&s1->leds, item, avl) {
		if (scene_led_from_path(s2, item->led->path))
			return true;
	}

	return false;
}

static const char *
state_str(enum scene_state state)
{
	struct scene_state_str {
		enum scene_state state;
		const char* str;
	};

	static struct scene_state_str scene_state_tbl[] = {
		{ SCENE_IDLE, "SCENE_IDLE" },
		{ SCENE_PAUSED, "SCENE_PAUSED" },
		{ SCENE_RUNNING, "SCENE_RUNNING" },
	};

	if (state > sizeof(scene_state_tbl))
		return "unknown";

	return scene_state_tbl[state].str;
}

static void
set_scene_state(struct scene *s, enum scene_state state)
{
	s->state = state;
	DEBUG(3, "%s to %s\n", s->name, state_str(state));
}

static void
stop_scene_leds(struct scene *s)
{
	struct scene_led *item;

	if (!is_running(s))
		return;

	avl_for_each_element(&s->leds, item, avl) {
		led_stop(led_from_path(item->led->path));
	}
}

static void
stop_scene(struct scene *s)
{
	stop_scene_leds(s);
	set_scene_state(s, SCENE_IDLE);
	led_timer_cancel(&s->timer);
}

static void
pause_scene(struct scene *s, struct scene *paused_by)
{
	stop_scene_leds(s);
	s->paused_by = paused_by;
	set_scene_state(s, SCENE_PAUSED);
	led_timer_cancel(&s->timer);
}

static bool
stage_scene(struct scene *s)
{
	struct scene_led *item;

	avl_for_each_element(&s->leds, item, avl) {
		led_add(item->led);
		led_run(led_from_path(item->led->path));
	}

	s->paused_by = NULL;
	set_scene_state(s, SCENE_RUNNING);
	led_timer_set(&s->timer, s->timeout);

	return true;
}

static bool
check_scene_priority(struct scene *run)
{
	struct scene *cur;

	avl_for_each_element(&scene_tree, cur, avl) {
		if (is_running(cur)) {
			if (!has_shared_leds(cur, run))
				continue;

			if (run->priority < cur->priority) {
				DEBUG(3, "pausing %s (prio=%d) < %s (prio=%d)\n",
				      run->name, run->priority, cur->name,
				      cur->priority);
				pause_scene(run, cur);
				return false;
			}

			pause_scene(cur, run);
		}
	}

	return true;
}

static bool
run_scene(struct scene *run)
{
	if (!check_scene_priority(run))
		return true;

	return stage_scene(run);
}

static void
resume_paused_scenes(struct scene *paused_by)
{
	struct scene *scene;

	avl_for_each_element(&scene_tree, scene, avl) {
		if (is_paused(scene) && (scene->paused_by == paused_by))
			run_scene(scene);
	}
}

static void
scene_timer_cb(struct led_timer *t)
{
	struct scene *s = container_of(t, struct scene, timer);

	stop_scene(s);
	resume_paused_scenes(s);
}

struct scene*
scene_add(const char *name, int timeout, int priority)
{
	char *_name;
	struct scene *scene = NULL;

	scene = calloc_a(sizeof(*scene), &_name, strlen(name) + 1);
	if (!scene)
		return NULL;

	avl_init(&scene->leds, avl_strcmp, false, NULL);
	scene->timeout = timeout;
	scene->priority = priority;
	scene->name = strcpy(_name, name);
	scene->avl.key = scene->name;
	scene->timer.cb = scene_timer_cb;

	if (!avl_insert(&scene_tree, &scene->avl)) {
		DEBUG(2, "add %s\n", scene_str(scene));
		return scene;
	}

	free(scene);
	return NULL;
}

static void
cleanup_scene(struct scene *s)
{
	struct scene_led *tmp;
	struct scene_led *item;

	avl_for_each_element_safe(&s->leds, item, avl, tmp) {
		avl_delete(&s->leds, &item->avl);
		blob_led_done(item->led);
		free(item);
	}

	free(s);
}

void
scene_led_add(struct scene *s, struct blob_led *b)
{
	struct scene_led *n = malloc(sizeof(struct scene_led));
	if (!n) {
		blob_led_done(b);
		return;
	}

	n->led = b;
	n->avl.key = b->path;
	avl_insert(&s->leds, &n->avl);
	led_add(b);
	DEBUG(3, "%s\n", blob_led_str(n->led));
}

bool
scene_run(const char *name)
{
	struct scene *new;

	new = avl_find_element(&scene_tree, name, new, avl);
	if (!new)
		return false;

	return run_scene(new);
}

bool
scene_stop(const char *name)
{
	struct scene *new;

	new = avl_find_element(&scene_tree, name, new, avl);
	if (!new)
		return false;

	stop_scene(new);
	resume_paused_scenes(new);
	return true;
}

static void
dump_scene_leds_blobmsg(struct blob_buf *b, struct scene *s)
{
	void *arr;
	struct scene_led *o;

	arr = blobmsg_open_array(b, "leds");
	avl_for_each_element(&s->leds, o, avl)
		led_state_blobmsg(b, led_from_path(o->led->path));
	blobmsg_close_table(b, arr);
}

void
scenes_state_blobmsg(struct blob_buf *b)
{
	void *arr;
	struct scene *s;

	arr = blobmsg_open_array(b, "scenes");

	avl_for_each_element(&scene_tree, s, avl) {
		void *t = blobmsg_open_table(b, "");

		blobmsg_add_string(b, "name", s->name);
		blobmsg_add_string(b, "state", state_str(s->state));
		blobmsg_add_u32(b, "timeout", s->timeout);
		blobmsg_add_u32(b, "priority", s->priority);

		dump_scene_leds_blobmsg(b, s);

		blobmsg_close_table(b, t);
	}

	blobmsg_close_table(b, arr);
}

const char*
scene_str(struct scene *s)
{
	static char buf[255] = {0};
	snprintf(buf, sizeof(buf), "%s priority=%d timeout=%d state=%s",
		 s->name, s->priority, s->timeout, state_str(s->state));
	return buf;
}

void
scene_done()
{
	struct scene *tmp;
	struct scene *scene;

	avl_remove_all_elements(&scene_tree, scene, avl, tmp) {
		cleanup_scene(scene);
	}
}
