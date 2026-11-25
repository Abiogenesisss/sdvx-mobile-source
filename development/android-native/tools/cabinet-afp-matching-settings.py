"""Original matching and option-screen AFP labels and dynamic bindings."""

from bemani.format import IFS

# The song matching archive's tex_bg_mat1 includes the SKIP strip. The
# arena matching archive uses the same texture name for a different image.
MATCHING_ARCHIVES = ("ready", "matching_music", "matching_music_ja",
                     "cmn_sys", "cmn_sys_ja", "psd_level", "psd_num", "play_data", "play_data_ja")
MATCHING_FIELDS = {"jak_usr": "song_jacket", "track_m_usr": "track_label", "level_usr": "level_yoko",
                   "bpm_usr": "song_bpm", "title_mat_usr": "song_title", "artist_mat_usr": "song_artist",
                   "effect_mat_usr": "song_effector", "illust_mat_usr": "song_illustrator"}
SETTINGS_HIDDEN = (
    "upper_plus_usr", "upper_usr", "standard_usr", "lower_usr", "off_position_usr", "early_late_all_usr",
    "early_late_critical_usr", "early_late_near_usr", "early_late_off_usr", "bg_100per_usr", "bg_75per_usr",
    "bg_50per_usr", "on_off_usr", "random_usr", "mirror_usr", "mirror_random_usr", "random_s_usr",
    "effective_rate_usr", "excessive_rate_usr", "kari_rate_usr", "deluxe_on_off_usr", "on_systemvoise_usr",
    "on_valkyrieeffect_usr", "on_rank_usr", "style_usr", "on_avg_usr", "challenge_off_usr",
    "challenge_target_usr", "challenge_puc_usr", "challenge_uc_usr", "target_matching_usr", "target_hiscore_usr",
    "target_s_usr", "target_aaaplus_usr", "target_aaa_usr", "new_usr", "all_usr", "all_konasute_usr", "ars_usr",
    "judge_sound_usr", "judge_draw_usr", "lanespeed_usr", "fix_usr", "judge_sound_slider_usr", "judge_draw_slider_usr",
    "lanespeed_slider_usr", "l_linecoler_yellow_usr", "l_linecoler_green_usr", "l_linecoler_blue_usr",
    "l_linecoler_red_usr", "r_linecoler_yellow_usr", "r_linecoler_green_usr", "r_lineclrer_brue_usr", "r_linecoler_red_usr",
)
COLORS = ("blue", "red", "green", "yellow")
SETTINGS_VALUES = (
    "standard", "upper", "upper_plus", "lower", "option_off", "option_on", "early_late_off", "early_late_near",
    "early_late_critical", "early_late_all", "bg_50per", "bg_75per", "bg_100per", "effective_rate", "excessive_rate",
    "ultra_rate", "random", "mirror", "mirror_rondom", "rondom_s", "lane_preview", "fix", "challenge_target",
    "challenge_puc", "challenge_uc", "challenge_hiscore", "challenge_s", "challenge_aaaplus", "challenge_aaa",
    "challenge_matching", "best_score_new", "best_score_all", "best_score_all_konasute",
    *(f"panel_linecoler_{color}" for color in COLORS),
)
SETTINGS_POPUPS = ("option_early_late_position", "option_plusminus_ms_style", "option_early_late_style", "option_bg",
                   "option_mirror", "option_panel_ratestyle", "option_challenge", "option_target", "option_best_score")
SETTINGS_CLIPS = {"in": {"start": "in", "end": "in_end"},
                  "in_end": {"start": "in_end", "preroll": "in", "frames": 1},
                  "out": {"start": "out", "end": "out_end"}}
SETTINGS_FOCUS = {f"panel_option_position_current{suffix}": "focus_frame" for suffix in ("", "_blue", "_yellow")}


def solid_keys(name, *shapes):
    return [f"shape_{name}_shape{shape}, untextured" for shape in shapes]


def setting_states(*labels):
    return {"clips": {
        **{label: {"start": label, "frames": 1} for label in labels},
        **{label + "_disabled": {"start": label, "frames": 1, "options": {"brightness": .5}} for label in labels},
    }, "options": {"texture_aliases": SETTINGS_FOCUS}}


SCENES = {
    "matching": (MATCHING_ARCHIVES, {
        "ready": {"clips": {"matching_in": {"start": "matching_in", "frames": 1200 - 53, "continuous": True}}},
        "maching_ue": {"clips": {
            "mat_in": {"start": "mat_in", "end": "mat_loop"},
            "mat_loop": {"start": "mat_loop", "preroll": "mat_in", "end": "mat_loop_end", "continuous": True},
            "mat_comp": {"start": "mat_comp", "end": "mat_out"},
        }, "options": {"texture_paths": MATCHING_FIELDS, "exclude": solid_keys("maching_ue", 5, 20),
                       "hidden_instances": [f"moni_{slot:02d}_usr" for slot in range(4)]}},
        "hispeed": {"clips": {
            "hispeed_in": {"start": "hispeed_in", "end": "hispeed_loop"},
            "hispeed_loop": {"start": "hispeed_loop", "preroll": "hispeed_in", "end": "hispeed_loop_end", "continuous": True},
            "hispeed_out": {"start": "hispeed_out", "end": "hispeed_out_end"},
        }, "options": {"exclude": solid_keys("hispeed", 5, 8, 9)}},
        "skip": {"clips": {"skip_loop": {"start": "skip_loop", "end": "skip_loop_end", "continuous": True}},
                 "options": {"exclude": solid_keys("skip", 5)}},
        "big_2_ready": {"clips": {"in": {"start": "in", "end": "in_end"},
                                   "in_end": {"start": "in_end", "preroll": "in", "frames": 1}},
                        "options": {"include": ["nameplate", "level_yoko"],
                                    "texture_paths": {"nameplate_usr": "nameplate", "level_usr": "level"}}},
    }, {"big_2_ready", "level_yoko", "arenaicon_25_u1", "nameplate", "text_chance_up", "title_supporter", "tradz"}),
    "settings": (("submonitor", "submonitor_ja", "psd_num"), {
        "page_option": {"clips": SETTINGS_CLIPS},
        **{f"page_option_{mode}": {"source": "page_option", "clips": SETTINGS_CLIPS,
            "options": {"hidden_instances": SETTINGS_HIDDEN + (() if mode == "premium" else
                ("option_challenge_base_usr", "option_avg_base_usr")),
                "exclude": solid_keys("page_option", 191, 498),
                "dimmed_instances": ["option_score_style_base_usr"] if mode == "light" else []}}
           for mode in ("normal", "light", "premium")},
        **{name: setting_states("select", "no_select") for name in SETTINGS_VALUES},
        "score_style": setting_states("score_ex", "score_nomal"),
        "option_top": {"clips": {label: {"start": label, "frames": 1} for label in ("press", "release")},
                       "options": {"texture_aliases": {"option": "option_tab_label"}}},
        **{name: {"clips": {"hide_to_disp": {"start": "hide_to_disp", "end": "disp_loop"},
                            "disp_loop": {"start": "disp_loop", "preroll": "hide_to_disp", "frames": 1}},
                  "options": {"hidden_instances": SETTINGS_HIDDEN}}
           for name in SETTINGS_POPUPS},
    }),
}


def copy_static(graphics, output):
    if output.name != "settings":
        return
    names = {name: name for name in ("option_gauge", "option_gauge_bar", "bt_rate_arson", "bt_rate_arsoff")}
    names["focus"] = "panel_option_position_current_blue"
    archive = IFS((graphics / "submonitor.ifs").read_bytes(), decode_textures=True)
    for target, source in names.items():
        (output / f"{target}.png").write_bytes(archive.read_file(f"tex\\{source}.png"))
