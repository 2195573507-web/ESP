#include "radar_rate_manager.h"

#include <string.h>

#define RADAR_RATE_IDLE_ENTER_MS 5000U

static const radar_rate_policy_t s_idle_policy = {100U, 200U, 1000U, 5000U, 0U};
static const radar_rate_policy_t s_detecting_policy = {100U, 100U, 333U, 1000U, 200U};
static const radar_rate_policy_t s_tracking_policy = {50U, 67U, 200U, 1000U, 100U};
static const radar_rate_policy_t s_fast_policy = {50U, 50U, 100U, 1000U, 67U};
static const radar_rate_policy_t s_lost_policy = {100U, 200U, 333U, 1000U, 0U};

static void set_mode(radar_rate_manager_t *manager, radar_rate_mode_t mode)
{
    manager->mode = mode;
    switch (mode) {
    case RADAR_RATE_DETECTING: manager->policy = s_detecting_policy; break;
    case RADAR_RATE_TRACKING: manager->policy = s_tracking_policy; break;
    case RADAR_RATE_FAST_MOVING: manager->policy = s_fast_policy; break;
    case RADAR_RATE_LOST_PENDING:
    case RADAR_RATE_LOST: manager->policy = s_lost_policy; break;
    case RADAR_RATE_IDLE:
    default: manager->policy = s_idle_policy; break;
    }
}

void radar_rate_manager_init(radar_rate_manager_t *manager, uint64_t now_ms)
{
    if (manager == NULL) return;
    memset(manager, 0, sizeof(*manager));
    manager->idle_since_ms = now_ms;
    set_mode(manager, RADAR_RATE_IDLE);
}

bool radar_rate_manager_update(radar_rate_manager_t *manager,
                               uint8_t candidate_count,
                               uint8_t active_count,
                               uint8_t retained_count,
                               uint64_t now_ms)
{
    if (manager == NULL) return false;
    const radar_rate_mode_t previous = manager->mode;

    if (active_count > 0U) {
        manager->idle_since_ms = 0U;
        set_mode(manager, RADAR_RATE_TRACKING);
    } else if (retained_count > 0U) {
        /* The tracker still owns the ID while its short occlusion hold is active. */
        manager->idle_since_ms = 0U;
        set_mode(manager, RADAR_RATE_LOST_PENDING);
    } else if (manager->previous_retained_count > 0U ||
               manager->mode == RADAR_RATE_LOST_PENDING) {
        /* The tracker released its last retained ID, so the loss is final. */
        manager->idle_since_ms = now_ms;
        set_mode(manager, RADAR_RATE_LOST);
    } else if (candidate_count > 0U) {
        manager->idle_since_ms = 0U;
        set_mode(manager, RADAR_RATE_DETECTING);
    } else if (manager->mode == RADAR_RATE_LOST && manager->idle_since_ms != 0U &&
               now_ms >= manager->idle_since_ms &&
               now_ms - manager->idle_since_ms < RADAR_RATE_IDLE_ENTER_MS) {
        /* Keep LOST observable briefly after deletion before returning to idle. */
    } else {
        if (manager->idle_since_ms == 0U || now_ms < manager->idle_since_ms) {
            manager->idle_since_ms = now_ms;
        }
        if (now_ms - manager->idle_since_ms >= RADAR_RATE_IDLE_ENTER_MS) {
            set_mode(manager, RADAR_RATE_IDLE);
        }
    }
    manager->previous_retained_count = retained_count;
    return previous != manager->mode;
}

const char *radar_rate_manager_mode_name(radar_rate_mode_t mode)
{
    switch (mode) {
    case RADAR_RATE_DETECTING: return "DETECTING";
    case RADAR_RATE_TRACKING: return "TRACKING";
    case RADAR_RATE_FAST_MOVING: return "FAST_MOVING";
    case RADAR_RATE_LOST_PENDING: return "LOST_PENDING";
    case RADAR_RATE_LOST: return "LOST";
    case RADAR_RATE_IDLE:
    default: return "IDLE";
    }
}
