"""Original generator and submonitor animations used by the cabinet."""


def frame(label=0):
    return {"start": label, "frames": 1}


def window(*, hidden=(), paths=None):
    options = {"hidden_instances": hidden, "texture_paths": paths or {}}
    return {"options": options, "clips": {
        "in": {"start": "in", "end": "in_end"},
        "in_end": {**frame("in_end"), "preroll": "in"},
        "out": {"start": "out", "end": "out_end", "preroll": "in"},
    }}


generator_hidden = ("text_usr", "num_pc_usr", "num_paseli_usr", "num_item_usr", "complete_usr", "new_usr",
                    "no_credit_usr")
generator_paths = {f"{name}_usr": key for name, key in (("banner", "banner"), ("item", "reward"),
                   ("base", "board_base"), ("sticker", "board_sticker"))}
generator_selector = window(hidden=generator_hidden, paths=generator_paths)
generator_selector["options"]["texture_paths"] = {**generator_paths, "fuda_gene_usr/banner_usr": "banner"}
generator_selector["options"]["exclude"] = ["shape_generator_sel_shape12, untextured"]

sub_text = ("title_usr", "artist_usr", "effector_usr", "illustrator_usr", "text1_usr", "text2_usr")
sub_cell = {**{f"ap_card_{i:02}_usr/apcard_usr": f"card_{i}" for i in range(1, 26)},
            **{f"board_{i:02}_usr/board_usr": f"board_{i}" for i in range(1, 4)}}
sub_page_hidden = ("set_date_usr", "button_save_usr", "button_cancel_usr", "favorite_usr", "select_item_usr",
                   "select_style_usr", "window_filter_usr", "window_item_usr", "button_select1_usr",
                   "button_select2_usr", "button_select3_usr", "button_select4_usr")
sub_window_hidden = (*sub_text, "favorite_usr", "select_item_usr", "select_style_usr")
sub_widgets = ("tab_home", "tab_crew", "tab_apicard", "tab_apiboard", "tab_achive", "tab_option", "tab_search",
               "button_save", "button_cancel")
sub_difficulties = {f"level_{index:02}_usr": f"level_{index}" for index in range(1, 6)}

SCENES = {
    "menu": (("cmn_bg", "cmn_sys", "cmn_sys_ja", "ms_sel", "ms_sel_ja", "psd_level", "psd_num"), {
        "bg_room": {"clips": {"60": {"start": 60, "frames": 240, "continuous": True}},
                    "options": {"exclude": ["shape_bg_room_shape25, untextured"]}},
        "cmn_title": {**window(hidden=("text_usr",)), "source": "title",
                      "options": {"include": ["cmn_titlebase"]}},
        **{name: window() for name in ("titletext_modesel", "titletext_my_room", "titletext_track_1st")},
        "operation_ms_sel": {"clips": {"operation_01": frame("operation_01")}},
        "ms_sel": {"clips": {"in_end": {**frame("in_end"), "preroll": "in"}}, "options": {
            "include": ["bg_temae", "base_info_track", "text_best_score_sel", "cursor_level", "tex_arrow_ultlevel",
                        "num_score_0", "box_score_max_ms", "box_score_ex_ms", "text_maxper_dot", "text_maxper"],
            "texture_paths": {f"info_usr/{str(10 ** index).zfill(8)}_usr": f"best_{index}" for index in range(8)},
            "texture_aliases": {"cursor_level": "cursor_level_4c75cdfe4e",
                "tex_arrow_ultlevel": "tex_arrow_ultlevel_238f6acdae", "box_score_max_ms": "box_score_max_ms_0e94613ae2",
                "box_score_ex_ms": "box_score_ex_ms_f14192046d", "text_maxper_dot": "text_maxper_dot_de17a059d3",
                "text_maxper": "text_maxper_1b04e1179b"},
        }},
        "cursor": {"clips": {"60": frame(60)}, "options": {
            "hidden_instances": ["mix_slot_usr"], "exclude": ["shape_cursor_shape8, untextured"]}},
    }, {"em6_s10_i_eab", "over_power_ms_sel", "radar_effect", "level_panel", "exkey_002"}),
    "entry_flow": (("game_over", "game_over_ja", "game_over_2025nabla", "cmn_sys", "cmn_sys_ja"), {
        **{name: window() for name in ("titletext_title_entry", "titletext_title_total_result", "titletext_track_premium")},
        "game_over_all": {"clips": {"in": {"start": "in", "end": "loop"},
            "loop": {"start": "loop", "frames": 240, "continuous": True, "preroll": "in"}},
            "options": {"include": ["251216_GAMEOVER", "font_kag", "font_tfyp", "light_1", "txt_game_over"]}},
        "2025nabla": {"clips": {"in": {"start": "in", "frames": 300}, "300": {**frame(300), "preroll": "in"}}},
    }),
    "generator_vii": (("generator_valkyrie", "generator_valkyrie_ja", "psd_num"), {
        "bg_gene": window(),
        "generator_sel": generator_selector,
        "app_generator_sel": {**generator_selector, "options": {**generator_selector["options"],
            # info_ticket_usr/num_usr is a green placeholder for the bound balance.
            "hidden_instances": (*generator_hidden, "num_usr"),
            "replace_instances": {"fuda_gene_usr": ["fuda_app", "fuda_app"],
                                  "gene_window_usr": ["app_window", "app_window"]}}},
        "fuda_app": {"clips": {}},
        "app_window": {"clips": {}},
        "anime_gene": {"clips": {"in": {"start": "in", "frames": 660}}},
        **{name: {"options": {"hidden_instances": generator_hidden, "texture_paths": generator_paths}, "clips": {
            "in": {"start": "in", "end": "loop"},
            "loop": {"start": "loop", "end": "out", "continuous": True},
            "out": {"start": "out", "end": "out_end"},
        }} for name in ("parts_get", "item_get")},
    }),
    "subscreen": (("submonitor", "submonitor_ja", "submonitor_myroom", "submonitor_myroom_ja",
                   "submonitor_achievement", "submonitor_achievement_ja", "psd_num", "psd_level",
                   "submonitor_cmn_window", "cmn_sys", "cmn_sys_ja"), {
        "common_frame": {"clips": {"0": frame()}, "options": {"hidden_instances": [
            *(f"{prefix}_{i:02}_usr" for prefix in ("tab", "notis") for i in range(1, 6)),
            "timer_premium_usr", "timer__usr", "tenkey_mini_usr"]}},
        "page_home": {"clips": {"0": frame()}},
        "myroom_home": window(),
        "page_music_search": window(hidden=(*sub_text, "search_fuda_list_usr"),
            paths={"music_info_usr/jacket_usr": "jacket", **{
                f"music_info_usr/{name}": key for name, key in sub_difficulties.items()}}),
        "keyboard_group": {"clips": {name: frame(name) for name in ("disp_loop", "hide_loop")}, "options": {
            "hidden_instances": ("keyboard_en_usr", "keyboard_ja_usr", "bg_usr", "searchbox_input_usr",
                                 "hitcount_usr", "cursor_usr")}},
        **{name: window(hidden=sub_page_hidden, paths={**sub_cell, "set_pdate_usr": "profile_preview"})
           for name in ("myroom_apica", "myroom_plate")},
        **{name: window(hidden=sub_window_hidden, paths={**sub_cell, "pinfo_usr": "profile"})
           for name in ("window_apica", "window_board", "window_style")},
        "window_item": window(hidden=("favorite_usr", "quantity_usr", "select_item_usr"),
            paths={f"stiker_L{i:02}_usr/item_l_usr": f"sticker_{i}" for i in range(1, 26)}),
        "date_apica": {"clips": {"0": frame()}, "options": {"hidden_instances": (*sub_text, "text_usr")}},
        "date_stiker": {"clips": {"0": frame()}, "options": {"hidden_instances": ("text1_usr", "text2_usr")}},
        "filter_apica": {"clips": {"0": frame()}, "options": {"hidden_instances": [
            *(f"button_genre_{i:02}_usr" for i in range(1, 7)),
            *(f"button_series_{i:02}_usr" for i in range(1, 8))]}},
        "page_achievement": window(hidden=("title_usr", "progress_text_usr", "progress_bar_usr", "ticket_all_usr",
                                           "folder_select_usr")),
        "folder_select": window(hidden=("folder_list_usr", "notis_change_usr", "notis_get_usr")),
        "win_sbm_common_small": window(),
        **{name: {"clips": {"operation_01": frame("operation_01")}}
           for name in ("operation_my_room", "operation_generator", "operation_generator_valkyrie")},
        **{name: {"clips": {"idle": {**frame(19), "preroll": 0}, "release": frame("release"),
                            "in": {"start": "in", "frames": 20}, "press": frame("press"),
                            "selected" if name.startswith("tab_") else "disabled": {
                                **frame(19), "preroll": 0, "options": {"brightness": .5}}}}
           for name in sub_widgets},
        **{name: {"clips": {"release": frame("release"), "press": frame("press")}} for name in
           ("hometab_crew", "hometab_apica", "hometab_plate", "button_A_style", "button_A_platebg",
            "button_A_sticker", "button_keyword", "button_getall", "button_change", "button_list_get")},
        **{name: {"clips": {"0": frame()}} for name in ("button_B_filter",
             "button_list_finish", "button_list_notyet")},
        "button_B_categoly01": {"clips": {"0": frame()}, "options": {"hidden_instances": ("text_usr", "num_usr")}},
        "button_B_favorit": {"clips": {state: frame(state) for state in ("off", "on")},
                            "options": {"hidden_instances": ("text_usr",)}},
        "button_categoly": {"clips": {"0": frame(), "off": {**frame(), "options": {
            "hidden_instances": ("text_usr", "num_usr", "cheak_usr")}}},
            "options": {"hidden_instances": ("text_usr", "num_usr")}},
        "search_box_fuda": {"clips": {state: frame(state) for state in ("touch", "release")}, "options": {
            "hidden_instances": (*sub_text, "text_usr", "icon_usr", "button_usr", "progress_usr", "notis_usr",
                                 "attention_usr", "cursor_l_usr"),
            "texture_paths": {"jacket_usr": "jacket", **sub_difficulties}}},
        **{name: window(hidden=(*sub_text, "text_usr", "icon_usr", "button_usr", "progress_usr", "notis_usr",
                                "attention_usr", "cursor_l_usr"))
           for name in ("select_item", "select_item_board", "select_style", "fuda_folder", "fuda_ticket", "cursor_l")},
        "progress_bar": {"clips": {"min": {"start": "min", "frames": 101}, "max": frame("max")}},
        "pop_new": {"clips": {"loop": {"start": "loop", "frames": 120, "continuous": True}}},
    }),
}
