"""Extract original cabinet AFP, GE2D and texture regions for the Android player."""

import argparse
from contextlib import redirect_stdout
from importlib import import_module
from io import StringIO
import json
from pathlib import Path
from xml.etree import ElementTree

from bemani.format import IFS
import bemani.format.afp.geo as geometry
from bemani.format.afp.swf import AP2DefineSpriteTag, AP2ImageTag
from bemani.format.dxt import DXTBuffer
from PIL import Image


GAUGES = ("gauge_effective_stagemain", "gauge_excessive_stagemain", "gauge_permissive_stagemain",
          "gauge_ultra_stagemain")
CHAIN_SLOTS = {f"chain_{'0' * digits}_usr": ["num_chain_a_0"] for digits in range(1, 5)}
CHAIN_OPTIONS = {"texture_slots": CHAIN_SLOTS,
                 # The chain digits are bound individually. The whole-number
                 # slot contains an untextured blue 216x60 authoring placeholder.
                 "hidden_instances": ["push_usr", "push_on_usr", "eff_shain_usr", "chain_center_usr", "chain_100_usr"]}
WINDOW_CLIPS = {"clips": {"in": {"start": "in", "end": "in_end"},
                          "in_end": {"start": "in_end", "frames": 1},
                          "out": {"start": "out", "end": "out_end"}}}
LOADING_CLIPS = {"loading_in": {"start": "loading_in", "end": "loading_in_end"},
                 "loading_loop": {"start": "loading_loop", "end": "loading_out", "continuous": True},
                 "loading_out": {"start": "loading_out", "end": "loading_out_end"}}
LOADING_UI = ("license_usr", "effect_visualizer_usr", "change_option_usr", "serected_usr",
              "label", "label 2", "label 3")
LOADING_SLOTS = [name + "_usr" for name in ("jacket", "bpm", "difficulty", "title", "artist", "effect", "illust")]
LOADING_ART = {"texture_aliases": {"track_final_i_eeb": "track"}}
ENTRY_HIDDEN = ["confirmation_new_usr", "yes_no_usr", "entry_09_scroll_usr", "pc_usr", "code_usr",
                "pin_usr", "text_usr", "play_data_usr", "make_usta_id_usr",
                *[f"moji_{i:02d}_usr" for i in range(1, 9)],
                *[f"name_eff_{i:02d}_usr" for i in range(1, 9)],
                *[f"moji{i}_frame_select_usr" for i in range(1, 11)]]
ENTRY_NAMES = ("entry_03", "entry_03_02", "entry_04", "entry_06", "entry_07", "entry_09")
PIN_PATHS = {f"{instance}_usr/key_{digit:02d}_usr": f"{slot}_{digit}"
             for instance, slot in (("pin", "pin"), ("new", "new"), ("confirmation", "confirm"))
             for digit in range(1, 5)}
FOLDER_HIDDEN = ["text_usr", "kyokusu_usr", "mark_usr", "grade_usr", "avg_usr", "hole_usr"]
SELECTION_ARCHIVES = ("ms_sel", "ms_sel_ja", "psd_level", "psd_num")
SELECTION_TARGETS = {"over_power_ms_sel", "radar_effect", "em6_s10_i_eab", "level_panel", "exkey_002"}


def solid_keys(name, *shapes):
    return [f"shape_{name}_shape{shape}, untextured" for shape in shapes]


RESULT_ARCHIVES = ("result", "result_ja", "psd_num")
RESULT_TARGETS = {"gauge_result", "small_2_result", "level_yoko", "nameplate", "skill_panel", "ap_all_nomal",
                  "psd_crew_0007", "base_0010", "item_nameplate_frame_001", "ap_all_floor", "force_playdate_s",
                  "radar_player"}
RESULT_HIDDEN = ["music_info_usr", "grade_usr", "option_usr", "score_list_usr", "player_data_usr",
                 "clear_gauge_usr", "clear_type_usr", "hantei_list_s_crit_usr", "hantei_graph_s_crit_usr",
                 "update_store_usr", "update_world_usr", "pt_option_usr", "pt_retry_usr", "blaster_gauge_usr",
                 "exgate_gauge_usr", "matching_usr", "ranking_usr", "blc_usr", "bonus_usr", "early_error_usr",
                 "early_near_usr", "crit_usr", "late_near_usr", "late_error_usr"]
RESULT_COUNTERS = {
    "hantei_list_default_usr": ("crit_chip", "near_chip", "error_chip", "crit_long", "crit_vol",
                                "error_long", "error_vol", "maximum"),
    "hantei_graph_default_usr": ("early_error", "early_near", "crit", "late_near", "late_error"),
}
RESULT_COUNTER_SLOTS = {
    f"play_result_usr/{group}/{counter}_num_usr/num_{'0' * (digit + 1)}_usr": f"{counter}_{digit}"
    for group, counters in RESULT_COUNTERS.items() for counter in counters for digit in range(4)
}
RESULT_SCORE_SLOTS = {
    f"{path}/num_{'0' * (digit + 1)}_usr": f"{slot}_{digit}"
    for path, slot in (("score_now_usr", "score"), ("score_best_usr/best_usr", "best"),
                       ("score_best_usr/diff_usr", "diff")) for digit in range(8)
}
ROOM_PANELS = {"panel_02_usr/panel_usr": "panel_08", "panel_03_usr/panel_usr": "panel_01",
               "panel_04_usr/panel_usr": "panel_04"}
ROOM_REPLACEMENTS = {path: [name, name] for path, name in ROOM_PANELS.items()}
SHINING_MOVIES = ("shining_aaa", "shining_aaa_plus", "shining_s")
MODE_CLIPS = {"in": {"start": "in", "end": "loop"},
              "loop": {"start": "loop", "frames": 60, "continuous": True, "hold_root": True},
              "out": {"start": "out", "end": "out_end"}}
MODE_HIDDEN = ["open_kac_usr", "open_arena_usr", "light_usr", "matching_usr", "none_usr"]
MODE_PATHS = {
    f"mode_{slot}_usr/{instance}": f"{key}_{slot}"
    for slot in (1, 2, 4, 5) for instance, key in (
        ("title_usr", "title"), ("text_usr", "description"), ("pay_usr/paseli_usr", "paseli"),
        ("pay_usr/credit_usr", "credit"),
    )
}
BUTTON_CLIPS = {
    label: {"start": label, "frames": frames, **({"continuous": True} if label.endswith("_loop") else {})}
    for label, frames in (("btn_error", 52), ("btn_near", 55), ("btn_critical", 35), ("btn_scritical", 35),
                          ("btn_in", 32), ("btn_loop", 240), ("btn_out", 32),
                          ("btnS_in", 32), ("btnS_loop", 240), ("btnS_out", 32))
}
LONG_CLIPS = {"FX_in": {"start": "FX_in", "end": "FX_loop"},
              "FX_loop": {"start": "FX_loop", "end": "FX_out", "continuous": True},
              "FX_out": {"start": "FX_out", "end": "FX_end"}}
BEAM_LABELS = ("beam_damage", "beam_near", "beam_critical", "beam_s_critical", "beam_end")
LASER_EFFECTS = {
    **{f"ef_vol_{side}": ("loop", 120) for side in ("l", "r")},
    **{f"ef_vol_{effect}_{side}": ("0", frames) for side in ("l", "r")
       for effect, frames in (("cri", 240), ("out", 50))},
    **{f"ef_vo_corner_{side}_{direction}": ("0", 20) for side in ("l", "r") for direction in ("l", "c", "r")},
    **{f"ef_cursor_{color}": ("0", 120) for color in ("green", "yellow")},
    **{f"{effect}_{color}": ("0", frames) for color in ("blue", "red", "green", "yell")
       for effect, frames in (("efhg_curve", 20), ("ef_line_hg", 120), ("efhg_dust", 80))},
}
CONSOLE_BUTTONS = tuple(f"io_button{number}" for number in range(1, 5)) + ("io_long1", "io_long2")
CONSOLE_KNOBS = ("io_vol_left", "io_vol_right")
CONSOLE_STYLES = ("", "_i", "_ii", "_iii", "_iv")
CONSOLE_EFFECTS = [f"io_eff_but{style}_{phase:02d}"
                   for style in (*CONSOLE_STYLES[1:], "_v") for phase in range(1, 4)]
CONSOLE_KNOB_EFFECTS = [f"io_eff_wa_{side}_{frame:02d}" for side in ("l", "R") for frame in range(14)]
PROFILE_PATHS = {
    f"{source}_usr": target for source, target in (
        ("canvas", "stamp_canvas"), ("frame", "nameplate_frame"), ("bg", "nameplate_background"),
        ("syougou", "player_title"), ("name", "player_name"), ("location", "player_location"),
        ("force", "player_force"), ("radar", "player_radar"), ("skill", "player_skill"),
        ("apica", "appeal_card"), ("crew", "crew_portrait"),
    )
}
CREW_ARCHIVES = ("submonitor", "submonitor_ja", "submonitor_myroom", "submonitor_myroom_ja", "psd_num")
CREW_GRIDS = {f"window_usr/window_item_usr/crewicon_{i:02d}_usr": f"icon_{i}" for i in range(1, 26)}
CREW_STYLES = {f"crew_{kind}{i}_usr": f"style_{kind}{i}"
               for kind, count in (("odd", 5), ("even", 6)) for i in range(1, count + 1)}
CREW_ICONS = {**CREW_GRIDS, **CREW_STYLES}
CREW_BUTTONS = ("button_save", "button_cancel")
CREW_TABS = ("tab_home", "tab_crew", "tab_apicard", "tab_apiboard", "tab_achive")
SCENES = {
    "shutter_nabla": (("shutter",), {"shutter_default": {"clips": {
        "in": {"start": "in", "end": "loop"},
        "loop": {"start": "loop", "end": "out", "continuous": True},
        "out": {"start": "out", "end": "out_end",
                "cue": [[name, "out"] for name in ("title_anime_usr", "parts_en_usr", "gate_usr")]},
    }}}),
    "title_original": (("title", "title_ja"), {"title": {"clips": {
        "in": {"start": "in", "end": "loop", "continuous": True},
    }}}),
    "caution_original": (("caution", "caution_ja"), {"caution": {"clips": {
        "0": {"start": 0, "frames": 400},
    }}}),
    "stage_ready": (("ready",), {"ready": {"clips": {
        "matching_out": {"start": "matching_out", "end": "game_start_end"},
    }}}),
    "hud": (("play_data", "game_hud", "game_hud_ja", "psd_num", "gauge"), {
        "hud_main": {
            "clips": {
                "hud_loop": {"start": "hud_loop", "frames": 1,
                             "options": {"hidden_instances": ["track_time_usr"]}},
            },
            "options": {"texture_slots": {f"score_{'0' * digits}_usr": ["num_score_0"] for digits in range(1, 9)}},
        },
        "hud_time": {"clips": {"music_start": {"start": "music_start", "frames": 1001}}},
        "small_1_stagemain": {"clips": {"0": {"start": 0, "frames": 1}},
                            "options": {"include": ["base_small_result_bg", "base_small_stagemain"]}},
        **{name: {"clips": {"0_per": {"start": "0_per", "frames": 501}},
                  "options": {"hidden_instances": ["percent_usr"]}} for name in GAUGES},
    }, {"base_player_info_02_gachi", "gauge_effective_stagemain", "jk_1580_5_ss", "small_1_stagemain", "level_yoko"}),
    "hud_chain": (("game_hud", "game_hud_ja", "psd_num"), {
        "chain": {"clips": {"0": {"start": 0, "frames": 1}}, "options": CHAIN_OPTIONS},
        "eff_chain": {
            "clips": {f"eff_combo_{i:02d}": {
                "start": f"eff_combo_{i:02d}", "end": f"eff_combo_{i + 1:02d}" if i < 5 else "eff_combo_neu",
            } for i in range(1, 6)},
            "options": CHAIN_OPTIONS,
        },
    }),
    "entry_wait": (("cmn_window", "cmn_window_ja"), {"win_connect": {
        **WINDOW_CLIPS, "options": {"texture_slots": ["text_usr"]},
    }}),
    "entry_checkin": (("cmn_window", "cmn_window_ja", "play_data"), {
        **{f"win_paseli_{color}": {**WINDOW_CLIPS, "options": {"hidden_instances": ["text_usr"]}}
           for color in ("gray", "green", "yellow")},
        "blpass_active": {"clips": {"0": {"start": 0, "frames": 1}}},
    }),
    "entry_original": (("card_entry", "card_entry_ja"), {
        **{name: {**WINDOW_CLIPS, "options": {
            "hidden_instances": ENTRY_HIDDEN,
            "exclude": ["txt_user_code"] if name == "entry_07" else [],
            "texture_aliases": {"name_moji_a": "name_selection"} if name == "entry_09" else {},
        }} for name in ENTRY_NAMES},
        "entry_02_id": {**WINDOW_CLIPS, "options": {"hidden_instances": ENTRY_HIDDEN}},
        "entry_09_scroll": {"clips": {"min": {"start": "min", "frames": 901}}},
        "name_eff": {"clips": {"loop": {"start": "loop", "frames": 120, "continuous": True}}},
        "confirmation_new": {"clips": {
            "choice_in": {"start": "choice_in", "end": "notselect_loop"},
            "new_loop": {"start": "new_loop", "end": "decide_new", "continuous": True},
            "transfer_loop": {"start": "transfer_loop", "end": "decide_transfer", "continuous": True},
            "decide_new": {"start": "decide_new", "end": "end_decide_new"},
            "decide_transfer": {"start": "decide_transfer", "end": "end_decide_transfer"},
        }},
        **{name: {"clips": {"0": {"start": 0, "frames": 1}}, "options": {"texture_paths": PIN_PATHS}}
           for name in ("pin_01", "pin_02")},
    }, {"big_1_wallet"}),
    "entry_profile": (("card_entry", "card_entry_ja"), {"entry_02_id": {
        "clips": {**WINDOW_CLIPS["clips"],
                  "in_end": {"start": "in_end", "frames": 10, "preroll": "in"}},
        "options": {"hidden_instances": ["code_usr", "make_usta_id_usr"],
                    "texture_aliases": {"big_1_wallet": "profile"}},
    }}, {"big_1_wallet"}),
    "track_intro": (("ms_sel", "ms_sel_ja"), {
        f"in_{track}": {"clips": {"in": {"start": "in", "end": "out"},
                                  "out": {"start": "out", "end": "out_end", "preroll": "in"}}}
        for track in ("1st", "2nd", "final", "ex", "premium")
    }),
    "track_header": (("cmn_sys", "cmn_sys_ja"), {
        f"titletext_track_{track}": {"clips": {"in_end": {"start": "in_end", "frames": 1}}}
        for track in ("2nd", "final", "ex")
    }),
    "cabinet_timer": (("cmn_sys", "cmn_sys_ja", "psd_num"), {
        **{name: {"clips": {
            "in_timer": {"start": "in_timer", "end": "stop_timer"},
            "loop_timer": {"start": "loop_timer", "end": "out_timer", "continuous": True},
            "stop_timer": {"start": "stop_timer", "frames": 1},
        }, "options": {"texture_slots": [f"timer_{digits}_usr"
                                         for digits in ("10", "01", "1000", "0100", "0010", "0001")]}}
           for name in ("time", "p_time")},
        "operation_card_entry": {"clips": {"operation_01": {"start": "operation_01", "frames": 1}}},
    }),
    "next_area": (("loading", "loading_ja", "psd_num", "psd_level"), {"loading": {
        "clips": LOADING_CLIPS,
        "options": {**LOADING_ART, "hidden_instances": LOADING_UI, "texture_slots": LOADING_SLOTS},
    }}, {"loading_mine2"}),
    "track_entry": (("loading", "loading_ja"), {
        "loading": {"clips": LOADING_CLIPS, "options": {"hidden_instances": [*LOADING_UI, *LOADING_SLOTS, "tarck_usr"]}},
        **{f"track_{name}": {"clips": {"loop": {"start": "loop", "frames": 1}}}
           for name in ("1st", "2nd", "final", "ex", "premium")},
    }, {"loading_mine2"}),
    "selection_ui": (SELECTION_ARCHIVES, {
        "lane_preview": {**WINDOW_CLIPS, "options": {"hidden_instances": ["preview_usr", "io_usr"],
                                                   "exclude": solid_keys("lane_preview", 8, 27)}},
        "sort": {**WINDOW_CLIPS, "options": {"hidden_instances": ["lamp_usr", "text_usr"],
                                           "exclude": solid_keys("sort", 8, 18)}},
        "window_select": {**WINDOW_CLIPS, "options": {
            "hidden_instances": ["title_usr", "artist_usr", "level_usr", "level_01_usr", "level_02_usr",
                                 "level_03_usr", "level_04_usr", "rate_usr", "random_mirror_usr",
                                 "lock_usr", "limit_usr", "input_usr"],
            "texture_paths": {"jacket_usr": "jacket"},
            "exclude": solid_keys("window_select", 19, 92),
        }},
        **{name: {**WINDOW_CLIPS, "options": {"hidden_instances": FOLDER_HIDDEN, "exclude": solid_keys(name, *guides)}}
           for name, guides in (("panel_folder", (8, 9, 12)), ("panel_folder_sub", (8, 9, 12, 15)))},
    }, SELECTION_TARGETS),
    "song_row": (SELECTION_ARCHIVES, {
        "panel_music": {"clips": {"60": {"start": 60, "frames": 1}}, "options": {
            "hidden_instances": ["blaster_bonus_usr", "select_usr", "new_usr", "lock_usr", "best_usr",
                                 "matching_usr", "base_matching_usr", "rec_usr", "fav_usr", "exkey_usr",
                                 "level_usr", "mark_usr", "grade_usr"],
            "texture_paths": {"jacket_usr": "jacket"},
            "exclude": solid_keys("panel_music", 8, 9, 51, 61),
        }},
        **{name: {"clips": {"0": {"start": 0, "frames": 1}},
                  "options": {"texture_slots": ["num_10_usr", "num_01_usr", "num_100_usr",
                                                "num_010_usr", "num_001_usr"]}}
           for name in ("panel_num_2", "panel_num_3", "sel_num_2", "sel_num_3")},
    }, SELECTION_TARGETS),
    "result_original": (RESULT_ARCHIVES, {
        "result_all_default": {"clips": {"in": {"start": "in", "end": "loop"},
                                          "loop": {"start": "loop", "frames": 1}},
                               "options": {"hidden_instances": RESULT_HIDDEN,
                                           "exclude": solid_keys("result_all_default", 8, 11, 46, 80, 83, 86, 233, 375, 397),
                                           "texture_paths": RESULT_COUNTER_SLOTS}},
        "music_info": {"clips": {"in": {"start": "in", "end": "in_end"},
                                "in_end": {"start": "in_end", "frames": 1}},
                       "options": {"texture_paths": {"title_usr": "song_title", "artist_usr": "song_artist",
                                                     "jacket_usr": "jacket", "level_usr": "level"}}},
        "grade": {"clips": {"loop": {"start": "loop", "frames": 1}},
                  "options": {"texture_paths": {"base_usr": "grade", "effect_usr": "grade_effect"}}},
        "option": {"clips": {"0": {"start": 0, "frames": 1}},
                   "options": {"texture_paths": {"random_mirror_usr": "notes_option", "gauge_type_usr": "rate_title"}}},
    }, RESULT_TARGETS),
    "result_score": (RESULT_ARCHIVES, {
        "normal_score_list": {"clips": {"0": {"start": 0, "frames": 1}}, "options": {
            "hidden_instances": ["ex_score_info_usr", "update_icon_usr"],
            "exclude": solid_keys("normal_score_list", 8),
            "texture_paths": {**RESULT_SCORE_SLOTS, "score_best_usr/best_usr/sign_usr": "best_sign",
                              "score_best_usr/diff_usr/sign_usr": "diff_sign"},
        }},
        "ex_score_info_hide": {"clips": {"0": {"start": 0, "frames": 1}}},
        **{f"clear_type_{name}": {"clips": {"0": {"start": 0, "frames": 100}}}
           for name in ("comp", "crash", "uc", "puc")},
    }, RESULT_TARGETS),
    "result_details": (RESULT_ARCHIVES, {
        "score_update": {"clips": {"in": {"start": "in", "frames": 31}},
                         "options": {"hidden_instances": ["score_old_usr", "score_new_usr"],
                                     "texture_aliases": {"box_update_world": "update_frame",
                                                         "text_update_world": "update_text"}}},
        "bonus_fuda": {"clips": {"0": {"start": 0, "frames": 1}},
                       "options": {"hidden_instances": ["bonus_text_usr", "bonus_num_usr"]}},
        "bpass_r_ef": {"clips": {"loop": {"start": "loop", "frames": 360, "continuous": True}}},
    }, RESULT_TARGETS),
    "result_gauge": (("gauge", "psd_num"), {
        f"gauge_{kind}_result": {"clips": {"0_per": {"start": "0_per", "frames": 501}},
                                "options": {"hidden_instances": ["num_usr"]}}
        for kind in ("effective", "excessive", "permissive", "ultra")
    }),
    "result_grade": (RESULT_ARCHIVES, {
        **{name: {"clips": {
            "in": {"start": "in", "end": "out_end"},
            "crash": {"start": "in", "end": "out_end", "options": {"replace_instances": {
                path: ["shining_track_crash", "shining_track_crash"]
                for path in ("shining_clear_type_usr", "shining_clear_type_usr 2")
            }}},
        }} for name in SHINING_MOVIES},
        "shining_track_crash": {"clips": {}},
    }, RESULT_TARGETS),
    "room_flow": (("my_room", "my_room_ja"), {
        **{f"room_{focus}": {
            "source": "my_room__",
            "options": {"replace_instances": ROOM_REPLACEMENTS, "hidden_instances": ["none_usr", "setsumei_usr"]},
            "clips": {
                "in": {"start": "in", "end": "loop", "continuous": True, "cue": [[path, "sentaku"]]},
                "loop": {"start": "loop", "frames": 120, "continuous": True, "hold_root": True,
                         "cue": [[path, "loop2"]]},
                "decide": {"start": "loop", "frames": 30, "hold_root": True, "cue": [[path, "loop2"]]},
                "out": {"start": "out", "end": "out_end", "cue": [[path, "loop2"]]},
            },
        } for focus, path in enumerate(ROOM_PANELS)},
        **{name: {"clips": {}} for name in ROOM_PANELS.values()},
        "cursor": {"clips": {"in": {"start": "in", "end": "loop", "continuous": True},
                             "decide": {"start": "decide", "end": "decide_end"},
                             "out": {"start": "out", "end": "out_end"}}},
    }),
    "stage_end": (("hantei",), {
        name: {"clips": {"0": {"start": 0, "frames": length}}}
        for name, length in (("complete", 440), ("crash", 440), ("perfect", 399),
                             ("perfect_s", 480), ("ultimate", 399))
    }),
    "cabinet": (("cmn_sys", "cmn_sys_ja", "submonitor", "submonitor_ja", "submonitor_myroom", "submonitor_myroom_ja"), {
        "operation_result": {"clips": {"operation_01": {"start": "operation_01", "frames": 1}}},
        "tab_option": {"clips": {"release": {"start": "release", "frames": 1}}},
        "hometab_crew": {"clips": {"press": {"start": "press", "frames": 1}}},
    }),
    "mode_flow": (("mode_credit", "mode_credit_ja"), {
        "mode_select": {"clips": MODE_CLIPS, "options": {
            "hidden_instances": [*MODE_HIDDEN, *(f"mode_{slot}_usr" for slot in (0, 3, 6, 7, 8))],
            "exclude": solid_keys("mode_select", 8),
            "texture_paths": MODE_PATHS,
        }},
        "mode_current": {"clips": MODE_CLIPS, "options": {
            "hidden_instances": MODE_HIDDEN,
            "exclude": solid_keys("mode_current", 8),
            "texture_paths": {"choice_usr/title_usr": "selected_title", "text_usr": "selected_description",
                              "choice_usr/pay_usr/paseli_usr": "selected_paseli",
                              "choice_usr/pay_usr/credit_usr": "selected_credit"},
        }},
    }),
    "button_hit": (("ver06/game_effect", "ver06/game_effect_hg"), {
        "ef_btn_all": {"clips": BUTTON_CLIPS},
        **{name: {"clips": LONG_CLIPS} for name in ("ef_BT_long", "ef_FX_long")},
        "lane_effect": {"clips": {"0": {"start": 0, "frames": 600}}},
    }),
    "beam": (("ver06/game_lane",), {
        f"beam_{number:02d}": {"clips": {
            start: {"start": start, "end": end} for start, end in zip(BEAM_LABELS, BEAM_LABELS[1:])
        }} for number in range(1, 5)
    }),
    "laser_effect": (("ver06/game_effect", "ver06/game_effect_hg"), {
        name: {"clips": {label: {"start": 0, "frames": frames,
                                **({"continuous": True} if label == "loop" else {})}}}
        for name, (label, frames) in LASER_EFFECTS.items()
    }),
    "laser_notice": (("game_hud", "game_hud_ja"), {
        f"vol_{side}_notice": {"clips": {
            "0": {"start": 0, "frames": 15},
            "shiji_r_loop": {"start": "shiji_r_loop", "frames": 55, "continuous": True},
            "shiji_r_out": {"start": "shiji_r_out", "frames": 20},
        }} for side in ("left", "right")
    }),
    "game_nemsys": (("game_nemsys",), {
        "io": {"clips": {"io_in": {"start": "io_in", "frames": 1}},
               "options": {"hidden_instances": ["nemsys_hantei_usr"]}},
        "hantei_line": {"clips": {"0": {"start": 0, "frames": 300}}},
    }),
    "stage_buttons": (("game_nemsys",), {
        **{f"{name}{style}": {"clips": {
            "btn_hit": {"start": 274, "frames": 32},
            "btn_push": {"start": 308, "frames": 1},
        }, "options": {"include": CONSOLE_EFFECTS}}
           for name in CONSOLE_BUTTONS for style in CONSOLE_STYLES},
        **{f"{name}{style}": {"clips": {
            "vol_hit_in": {"start": 274, "frames": 32},
            "vol_loop": {"start": 306, "frames": 22},
            "vol_notice": {"start": 330, "frames": 16,
                           "options": {"include": [name + suffix for suffix in (*CONSOLE_STYLES[1:], "_v")]}},
        }, "options": {"include": CONSOLE_KNOB_EFFECTS}}
           for name in CONSOLE_KNOBS for style in CONSOLE_STYLES},
    }),
    "session_dialog": (("cmn_window", "cmn_window_ja", "yes_no"), {
        **{name: {**WINDOW_CLIPS, "options": {
            "hidden_instances": ["text_usr", "yes_no_usr"] if name == "win_yes_no" else ["text_usr"]}}
           for name in ("win_yes_no", "win_connect")},
        "yes_no": {"clips": {
            "choice_in": {"start": "choice_in", "end": "notselect_loop"},
            "yes_loop": {"start": "yes_loop", "frames": 60, "continuous": True},
            "no_loop": {"start": "no_loop", "frames": 60, "continuous": True},
            "decide_yes": {"start": "decide_yes", "end": "end_decide_yes"},
            "decide_no": {"start": "decide_no", "end": "end_decide_no"},
        }},
    }),
    "payment": (("mode_credit", "mode_credit_ja", "cmn_sys", "cmn_sys_ja"), {
        "paseli_credit": {"clips": {
            "in": {"start": "in", "end": "loop"},
            "loop": {"start": "loop", "frames": 1},
            "out": {"start": "out", "frames": 35},
        }, "options": {"include": ["6_pay_text_05", "tex_win_line", *solid_keys("paseli_credit", 138, 139, 140)]}},
        "bg_mat": {"clips": {"in": {"start": "in", "end": "in_end"},
                             "in_end": {"start": "in_end", "frames": 1}}},
        "operation_mode_credit": {"clips": {
            label: {"start": label, "frames": 1} for label in ("operation_01", "operation_02")
        }},
        **{name: {"clips": {"0": {"start": 0, "frames": 1}}}
           for name in ("fuda_credit", "fuda_back", "pay_credit", "pay_s_credit",
                        *(f"mode_text_{i:02d}" for i in range(1, 10)))},
    }),
    "result_force": (("force", "ver06/force_ja", "psd_num"), {
        **{f"force_{kind}{suffix}": {"clips": {"in": {"start": "in", "end": "loop"}},
                                    "options": {"hidden_instances": ["scouter_add_usr", "scouter_usr"]}}
           for kind in ("plus", "level_up") for suffix in ("", "bg")},
        "class_up": {"clips": {"in": {"start": "in", "end": "out"}, "out": {"start": "out", "frames": 108}},
                     "options": {"hidden_instances": ["scouter_add_usr", "scouter_usr"],
                                 "texture_paths": {"em6_before_usr": "emblem_before", "em6_after_usr": "emblem_after"}}},
        "force_playdate_s": {"clips": {"0": {"start": 0, "frames": 1}}, "options": {"hidden_instances": [
            "force_num_usr", "force_star_usr", "force_star_up_usr", "flash_up2_usr", "flash_up3_usr", "flash_up4_usr",
        ]}},
        **{f"star_{color}_{i}": {"clips": {"0": {"start": 0, "frames": 1}}}
           for color in ("silver", "gold") for i in range(1, 5)},
    }),
    "result_profile": (("nameplate", "psd_num", "psd_stamp_bg"), {
        name: {"clips": {"0": {"start": 0, "frames": 1}},
               "options": {"hidden_instances": ["profile_card_usr"], "texture_paths": PROFILE_PATHS}}
        for name in ("nameplate", "profile_card_01_enjoy", "profile_card_02_gachi")
    }, RESULT_TARGETS),
    "player_card": (("ver06/play_data_small",), {
        "matching_mine": {"clips": {"mine_in": {"start": "mine_in", "frames": 180},
                                   "mine_loop": {"start": "mine_loop", "frames": 45, "continuous": True}},
                          "options": {"exclude": solid_keys("matching_mine", 5),
                                      "texture_aliases": dict(zip(solid_keys("matching_mine", 29, 32, 35),
                                                                  ("player_title", "player_location", "player_name")))}},
        "loading_mine": {"clips": {"0": {"start": 0, "frames": 1}}},
        "stagemain_mine": {"clips": {"hud_loop": {"start": "hud_loop", "frames": 1}},
                           "options": {"exclude": solid_keys("stagemain_mine", 5, 8, 19, 43)}},
    }, RESULT_TARGETS | {"arenaicon_11_b2", "skill_panel2"}),
    "crew_selector/background": (("submonitor",), {
        "bg": {"clips": {"0": {"start": 0, "frames": 9000, "continuous": True, "hold_root": True}}},
    }),
    "crew_selector": (CREW_ARCHIVES, {
        "bg_sub_myroom": {"clips": {"0": {"start": 0, "frames": 1}}},
        "select_crew": {**WINDOW_CLIPS},
        "myroom_crew": {"clips": {
            **WINDOW_CLIPS["clips"],
            "in": {"start": "in", "frames": 60},
            "in_end": {"start": "in_end", "frames": 1, "preroll": "in"},
        },
            "options": {
                "replace_instances": {path: ["myroom_crew", "crewicon"] for path in CREW_STYLES},
                "texture_paths": {"psr_crew_usr": "portrait", "name_usr": "name",
                                  **{f"{path}/crew_icon_usr": binding for path, binding in CREW_ICONS.items()}},
                "texture_instances": {**{f"{path}/select_item_usr": f"focus_{binding}"
                                         for path, binding in CREW_ICONS.items()},
                                      **{f"{name}_usr": name for name in CREW_BUTTONS}},
                "hidden_instances": [
                    "window_voice_usr", "window_filter_usr", "button_filter_usr", "button_voice_usr",
                    "icon_voice_usr", "favorite_usr",
                    "timer_premium_usr", "timer__usr", "tenkey_mini_usr", "yaji_L_usr", "yaji_R_usr",
                    *(f"button_select{i}_usr" for i in range(1, 5)),
                    *(f"tab_{i:02d}_usr" for i in range(1, 6)),
                    *(f"notis_{i:02d}_usr" for i in range(1, 6)),
                ],
            }},
        **{name: {**WINDOW_CLIPS} for name in ("myroom_home", "window_crew")},
        "common_frame": {"clips": {"0": {"start": 0, "frames": 1}}},
        "hometab_crew": {"clips": {label: {"start": label, "frames": 1} for label in ("release", "press")}},
        **{name: {"clips": {
            "release": {"start": "release", "frames": 10},
            "press": {"start": "press", "frames": 1},
            "idle": {"start": 19, "frames": 1, "preroll": "release"},
            "selected": {"start": 19, "frames": 1, "preroll": "release", "options": {"brightness": .5}},
        }} for name in CREW_TABS},
        **{name: {"clips": {
            "press": {"start": "press", "frames": 1},
            "idle": {"start": 15, "frames": 1, "preroll": "release"},
            "disabled": {"start": 15, "frames": 1, "preroll": "release", "options": {"brightness": .5}},
            "release_disabled": {"start": "release", "frames": 6, "options": {"brightness": .5}},
        }} for name in CREW_BUTTONS},
    }, {"psd_crew_0007"}),
}


SCENES.update(import_module("cabinet-afp-auxiliary").SCENES)
MATCHING_SETTINGS = import_module("cabinet-afp-matching-settings")
SCENES.update(MATCHING_SETTINGS.SCENES)
SKILL_TOTAL = import_module("cabinet-afp-skill-total")
SCENES.update(SKILL_TOTAL.SCENES)


def prepare(graphics, output, archives, movies, runtime_textures=()):
    menu = import_module("compile-menu")
    geometry.descramble_text = import_module("compile-stage-background").geometry_name
    textures = {}
    sources = {}
    for name in archives:
        path = (graphics.parent if "/" in name else graphics) / f"{name}.ifs"
        archive = IFS(path.read_bytes(), decode_binxml=True)
        for file in archive.filenames:
            if file.startswith("afp\\bsi\\"):
                sources[file.split("\\")[-1]] = archive
        if "tex\\texturelist.xml" not in archive.filenames:
            continue
        index = ElementTree.fromstring(archive.read_file("tex\\texturelist.xml"))
        for atlas in index.findall("texture"):
            for entry in atlas.findall("image"):
                textures[entry.attrib["name"]] = (archive, atlas.attrib, entry)

    for directory in ("afp/bsi", "geo", "tex"):
        (output / directory).mkdir(parents=True, exist_ok=True)
    copied = set()
    needed = set()

    def image_tags(timeline):
        for tag in timeline.tags:
            if isinstance(tag, AP2ImageTag):
                needed.add(tag.reference)
            elif isinstance(tag, AP2DefineSpriteTag):
                image_tags(tag)

    def copy_movie(name):
        if name in copied:
            return
        copied.add(name)
        archive = sources[name]
        afp = archive.read_file(f"afp\\{name}")
        bsi = archive.read_file(f"afp\\bsi\\{name}")
        movie = menu.MenuSWF(name, afp, bsi)
        with redirect_stdout(StringIO()):
            movie.parse()
        image_tags(movie)
        (output / "afp" / name).write_bytes(afp)
        (output / "afp/bsi" / name).write_bytes(bsi)
        for file in archive.filenames:
            if not file.startswith(f"geo\\{name}_shape"):
                continue
            shape_name = file.split("\\")[-1]
            raw = archive.read_file(file)
            shape = menu.menu_shape(shape_name, raw)
            shape.parse()
            (output / "geo" / shape_name).write_bytes(raw)
            for draw in shape.draw_params:
                if not draw.flags & 1:
                    break
                if draw.flags & 2:
                    needed.add(draw.region)
        for dependency in movie.imported_tags.values():
            if dependency.swf.split(".")[0] not in ("aeplib", "asdlib"):
                copy_movie(dependency.swf)

    for name, settings in movies.items():
        copy_movie(settings.get("source", name))
    regions = {}
    for name in sorted(needed):
        if name in runtime_textures:
            regions[name] = {"runtime": True}
            continue
        archive, atlas, entry = textures[name]
        img = [int(v) // 2 for v in entry.findtext("imgrect").split()]
        uv = [int(v) / 2 for v in entry.findtext("uvrect").split()]
        size = (img[1] - img[0], img[3] - img[2])
        raw = archive.read_file("tex\\" + name)
        if atlas["format"] == "argb8888rev":
            rgba = Image.frombytes("RGBA", size, raw.ljust(size[0] * size[1] * 4, b"\0"), "raw", "BGRA")
        elif atlas["format"] == "dxt5":
            rgba = Image.frombytes("RGBA", size, DXTBuffer(*size).DXT5Decompress(raw, swap=True))
        else:
            raise ValueError(f"Unsupported AFP texture format {atlas['format']} for {name}")
        rgba.save(output / "tex" / f"{name}.png")
        regions[name] = {"source_rect": [uv[0] - img[0], uv[2] - img[2], uv[1] - uv[0], uv[3] - uv[2]],
                         "nearest": atlas["mag_filter"] == "nearest"}
    (output / "afp-textures.json").write_text(json.dumps(regions, indent=2) + "\n", encoding="utf-8")
    (output / "afp-library.json").write_text(json.dumps(movies, indent=2) + "\n", encoding="utf-8")
    print(f"{output.name}: {len(copied)} original AFP, {len(regions)} texture regions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path, help="Original graphics/ver07 directory")
    parser.add_argument("assets", type=Path, help="Android assets directory")
    parser.add_argument("scenes", choices=tuple(SCENES), nargs="+")
    args = parser.parse_args()
    for scene in args.scenes:
        output = args.assets / scene
        prepare(args.graphics, output, *SCENES[scene])
        if scene in MATCHING_SETTINGS.SCENES:
            MATCHING_SETTINGS.copy_static(args.graphics, output)
        SKILL_TOTAL.copy_static(args.graphics, output, scene)
