"""Original NABLA course selection, course effects and total-result movies."""

from bemani.format import IFS

SKILL_ARCHIVES = ("skill_sel", "skill_sel_ja", "ver06/psd_skill", "psd_num", "psd_level", "cmn_sys", "cmn_sys_ja")
TOTAL_ARCHIVES = ("total_result", "total_result_ja", "cmn_sys", "cmn_sys_ja", "psd_num", "psd_level")
COURSE_FIELDS = (
    "past_status_usr", "icon_usr", "b_mat_usr", "b_new_usr", "skill_usr", "mes_usr", "course_name_usr",
    "total_ex_usr", "past_grade_usr", "grade_usr", "gauge_usr",
    *(f"title_{index:02d}_usr" for index in range(1, 5)),
)
COURSE_BINDINGS = {
    "jacket_usr": "jacket",
    **{f"level_bg_{index:02d}_usr": f"level_{index}" for index in range(1, 5)},
}


def guide_shape(movie, shape):
    return f"shape_{movie}_shape{shape}, untextured"


WINDOW_CLIPS = {"clips": {
    "in": {"start": "in", "end": "in_end"},
    "in_end": {"start": "in_end", "frames": 1, "preroll": "in"},
    "out": {"start": "out", "end": "out_end"},
}}

SCENES = {
    "skill_selector": (SKILL_ARCHIVES, {
        "bg_skill": {"clips": {"normal": {"start": "normal", "end": "nomal_to_god", "continuous": True}}},
        **{name: {"clips": {
            "in": {"start": "in", "end": "in_end"},
            "in_end": {"start": "in_end", "frames": 1, "preroll": "in"},
        }, "options": {"hidden_instances": (*COURSE_FIELDS, "text_usr"),
                       "exclude": [guide_shape(name, 8)], "texture_paths": COURSE_BINDINGS}}
           for name in ("big_frame", "small_frame")},
        "operation_skill_analyzer": {"clips": {"operation_01": {"start": "operation_01", "frames": 1}}},
        "titletext_skill_analyzer": {"clips": {"in_end": {"start": "in_end", "frames": 1}}},
    }, {"gauge_achievement", "jk_1580_5_s", "jk_1580_5_ss", "skill_panel2"}),
    "skill_effects": (("skill_sel", "skill_sel_ja", *TOTAL_ARCHIVES), {
        "in_skill_sel": {"clips": {"in": {"start": "in", "end": "skill_all_in"}}},
        **{name: {"clips": {"in": {"start": "in", "end": "total_result_in"}}}
           for name in ("in_success", "in_fail")},
        **{name: {"clips": {"loop": {"start": "loop", "frames": 1}}}
           for name in ("hantei_success", "hantei_fail")},
        "skill_change": {"clips": {"in": {"start": "in", "end": "end"}},
                         "options": {"texture_paths": {"skill_00_usr": "before", "skill_01_usr": "after"}}},
        "skill_total_info": {"clips": {"0": {"start": 0, "frames": 1}},
                             "options": {"hidden_instances": ["hantei_usr", "total_exscore_usr", "gauge_usr"],
                                         "texture_paths": {"skill_usr": "skill",
                                                           **{f"per_{i}_usr": f"rate_{i}" for i in range(3)}}}},
    }, {"skill_panel", "skill_panel_kami"}),
    "skill_effects/windows": (("cmn_window", "cmn_window_ja", "yes_no", "psd_num"), {
        "win_skill_change": {**WINDOW_CLIPS,
                             "options": {"hidden_instances": ["skill_before_usr", "skill_after_usr", "text_usr", "yes_no_usr"]}},
        "win_skill_bonus": {**WINDOW_CLIPS,
                            "options": {"texture_paths": {f"bonus_p{i:02d}_usr": f"bonus_{i}" for i in range(1, 6)}}},
        "yes_no": {"clips": {name: {"start": name, "end": name + "_end"} for name in ("no_yes", "yes_no")}},
    }, {"skill_panel", "skill_panel_kami", "yes_no"}),
    "ex_track": (("ms_sel", "ms_sel_ja"), {
        "gauge_blaster": {"clips": {"0": {"start": 0, "frames": 1}},
                          "options": {"hidden_instances": ["bpass_base_usr", "bpass_ef_usr"]}},
        "bpass_r_ef": {"clips": {"loop": {"start": "loop", "frames": 60, "continuous": True}}},
    }, {"over_power_ms_sel"}),
    "total_result": (TOTAL_ARCHIVES, {
        "total_all": {"clips": {"in": {"start": "in", "end": "loop"},
                                "loop": {"start": "loop", "frames": 240, "continuous": True}},
                      "options": {
                          "hidden_instances": ["fuda01_usr", "fuda02_usr", "fuda03_usr", "fuda04_usr",
                                               "fuda05_usr", "play_data_usr", "ticket_bpl_usr", "ap_usr"],
                          "texture_paths": {
                              f"{instance}/num_{size}{digit:02d}_usr": f"{kind}_{amount}_{digit - 1}"
                              for instance, kind in (("pc_usr", "packet"), ("blc_usr", "block"))
                              for size, amount in (("b", "total"), ("s", "gain")) for digit in range(1, 9)
                          },
                      }},
        "result_data": {"clips": {"in": {"start": "in", "end": "in_end"},
                                  "in_end": {"start": "in_end", "frames": 1, "preroll": "in"}},
                        "options": {
                            "hidden_instances": ["grade_usr", "icon_usr", "level_usr", "track_usr", "ex_score_usr", "title_usr"],
                            "exclude": [guide_shape("result_data", 8)],
                            "texture_paths": {f"{10 ** digit:08d}_usr": f"score_{digit}" for digit in range(8)},
                        }},
        **{f"track_num_{name}": {"clips": {"0": {"start": 0, "frames": 1}}}
           for name in ("1st", "2nd", "3rd", "final", "ex", "over")},
        "operation_total_result": {"clips": {"operation_01": {"start": "operation_01", "frames": 1}}},
    }, {"big_1_wallet", "level_yoko"}),
}


STATIC_TEXTURES = {
    "skill_selector": {"ver06/psd_skill": tuple(f"skill_back_panel_{i:02d}" for i in range(13))},
    "skill_effects/windows": {"psd_num": tuple(f"num_stamp_bonus_{i}" for i in range(10))},
    "ex_track": {"ms_sel": ("bpass_r_icon",), "ms_sel_ja": ("pt_playend",)},
    "total_result": {"psd_num": tuple(f"num_{font}_{i}" for font in ("score", "bonus") for i in range(10))},
}


def copy_static(graphics, output, scene):
    for archive_name, names in STATIC_TEXTURES.get(scene, {}).items():
        path = (graphics.parent if "/" in archive_name else graphics) / f"{archive_name}.ifs"
        archive = IFS(path.read_bytes(), decode_textures=True)
        for name in names:
            (output / f"{name}.png").write_bytes(archive.read_file(f"tex\\{name}.png"))
