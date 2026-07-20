#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_file="$test_dir/../../network_worker/network_worker.c"

awk '
    /const bool entering_link_stable =/ {
        in_transition = 1
    }
    in_transition && /if \(entering_link_stable\) \{/ {
        in_entry = 1
    }
    in_entry && /network_worker_enqueue_home_ai_rule_sync\(\)/ {
        sync_calls++
    }
    in_entry && /^    }$/ {
        in_entry = 0
    }
    in_transition && /network_replay_worker_request_bme_replay\(\);/ {
        replay_calls++
        in_transition = 0
    }
    END {
        if (sync_calls != 1 || replay_calls != 1) {
            exit 1
        }
    }
' "$source_file"

awk '
    /static esp_err_t enqueue_command_work_item\(/ {
        in_enqueue = 1
    }
    in_enqueue && /NETWORK_WORKER_WORK_HOME_AI_RULE_SYNC && s_home_ai_rule_sync_pending/ {
        found_dedupe = 1
    }
    in_enqueue && /^}$/ {
        in_enqueue = 0
    }
    END {
        if (!found_dedupe) {
            exit 1
        }
    }
' "$source_file"

awk '
    /static bool release_disconnected_session\(/ {
        in_release = 1
    }
    in_release && /if \(released\) \{/ {
        released_gate = 1
    }
    in_release && released_gate && /home_ai_voice_session_release_owner\(device_id\)/ {
        found_owner_release = 1
    }
    in_release && /^}$/ {
        in_release = 0
    }
    END {
        if (!found_owner_release) {
            exit 1
        }
    }
' "$source_file"

printf '%s\n' "home ai network recovery contract tests: PASS"
