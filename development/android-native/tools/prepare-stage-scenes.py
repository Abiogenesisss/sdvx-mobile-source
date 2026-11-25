"""Export the original song scenes from extracted game_bg IFS packages.

Playback labels come from the 2026101300 AFPs. Scene names/scales are registered
by soundvoltex.dll at 18048d219, 1804907a0 and 180494462. EXBGTEX uses the
original tex%02d / text_%02d replacement families.
"""
import argparse
import json
from pathlib import Path
import shutil
from importlib import import_module


def layer(animation="", label="", **options):
    return dict(animation=animation, label=label, **options)


def standard_scene(archive, animation, transitions, *, upper=False, texture_packs=()):
    initial = [layer("bg_all", "normal")]
    if upper:
        initial.append(layer("bg_top", label=0))
    events = {event: [layer(animation, label, slot=len(initial))]
              for event, label in transitions.items()}
    return dict(archive="gmbg_" + archive, initial=[*initial, layer()], events=events,
                gauge_layer=0, texture_packs=texture_packs)


def label_tracks(prefix, tracks):
    return {f"x{prefix + index:08x}": (path, names) for index, (path, names) in enumerate(tracks)}


def numbered_labels(*indices):
    return {i: "in" if i == 0 else f"in{i}" for i in indices}


GATE_PATH = "usr_maingate/usr_gate"


RAY_LASERS = ("a3_r", "a2_r", "a1_r", "a1_l", "a2_l", "a3_l",
              "b_r", "c_r", "c_l", "b_l", "shipef_r", "shipef_l")
RAY_PATHS = [f"airship_usr/laser_{name}_usr" for name in RAY_LASERS]
RAY_LABELS = [(f"{path}/laser_x{name[:2]}_usr" if i < 6 else path, ("in", "out", "in2"))
              for i, (path, name) in enumerate(zip(RAY_PATHS, RAY_LASERS))]
RAY_LABELS += [("chara_shion_usr", ("in", "out")), ("/", ("in", "in2", "out", "in3", "out2"))]
MIXXION_BG_SLOTS = {slot: ["shape_mixxion_shape171, untextured"] for slot in (
    "bg_base_usr", "bg0_usr", "bg1_usr", "bg2_usr", "bg3_usr", "ana_usr",
    "ana_migi_usr", "ana_hidari_usr", "ana_migi2_usr", "ana_hidari2_usr", "ana_saigo_usr")}
MAXIMA_PROMPT_TEXTURES = ("c_max_koe1_i_eab", "c_max_koe2_i_eab")
SCENES = {
    # Resource IDs and layers registered by 180485ea0. Playback uses original AFP labels.
    271: dict(archive="gmbg_exgate_vallisneria", texture_packs=("gmbg_exgate",), chart_timelines=[dict(animation="bg_vallisneria", slot=0, intro="in", labels=label_tracks(0, (
            ("/", numbered_labels(0, 4, 5)), ("usr_vallis", numbered_labels(0, 6, 7)),
            ("usr_eff", numbered_labels(0, 4, 7)), ("usr_maingate", numbered_labels(0, 2)),
            (GATE_PATH, numbered_labels(0, 1, 4, 7)),
            (GATE_PATH + "/usr_window", {0: "in1", **numbered_labels(1, 2, 3, 5, 7)}))))],
        initial=[layer("bg_vallisneria","in"),layer(),layer(after_models=True)],
        events={"IN6":[layer("bg_vn_vallis","in6",slot=1)],
                "IN7":[layer("bg_vn_vallis","in7",slot=1),layer("bg_vn_eff_on","in7",slot=2)]}),
    551: standard_scene("nishinippori", "bg_maxima", {"IN": 'in', "IN2": 'in2', "IN3": 'in3', "OUT": 'out'}),
    610: standard_scene("303boss", "bg_all_sp", {"BASE_IN1": 'in1', "BASE_IN2": 'in2', "BASE_OUT": 'out'}),
    # bg_no 34 registers the star tunnel and a separate bg_maxima EXBG movie
    # at 18048b62e and 18048b7e3. VOX OFF/ON controls that movie.
    661: dict(archive="gmbg_2015_kac01", gauge_layer=0, tunnels=[("star", 67.5, 3, 8)],
        initial=[layer("bg_all", "normal"), layer(after_models=True)],
        events={"OFF": [layer(slot=1)], "ON": [layer("bg_maxima", "in", slot=1)]}),
    # bg_no 44 and 50 register these second AFPs independently of bg_all.
    709: dict(archive="gmbg_tenkaichi02", gauge_layer=0, initial=[layer("bg_all", "normal"), layer("bg_maxima", "non")], events={}),
    939: dict(archive="gmbg_jomanda", gauge_layer=0, initial=[layer("bg_all", "normal"), layer("hame_loop", "loopl", after_models=True)], events={}),
    636: dict(archive="gmbg_exgate_everlast", texture_packs=("gmbg_exgate",), chart_timelines=[dict(animation="bg_everlast", slot=0, intro="in", labels=label_tracks(0, (
            ("usr_eff", numbered_labels(0, 1, 4, 6, 7)), ("usr_maingate", numbered_labels(0, 2)),
            (GATE_PATH, numbered_labels(0, 2, 4, 6, 7)), (GATE_PATH + "/usr_window", numbered_labels(0, 2, 3)),
            ("usr_rasis", numbered_labels(0, 4, 6)), ("usr_rasis/usr_rasis_a", numbered_labels(0, 5)),
            ("usr_rasis/usr_rasis_b/usr_rasis_b2", numbered_labels(0, 7)))))],
        initial=[layer("bg_everlast","in")], events={}),
    # Sayonara Planet Wars XCD: 1804a11f6–1804a168f registers the
    # pop/space/empty tunnels and the paired Tama/space AFPs.
    642: dict(archive="gmbg_pop_tama",
        tunnels=[["pop", 45, 2, 4], ["space", 45, 2, 4], None],
        initial=[layer("bg_all", "normal", priority=66, options={"folder": "stage_bg/4"}),
                 layer(), layer()], gauge_layer=0,
        events={f"{prefix}IN{i}": [layer(name, f"in{i}", slot=slot)]
                for slot, (prefix, name) in enumerate((("EX2_", "bg_space"), ("", "bg_tama")), start=1)
                for i in (1, 2, 3, 5)}),
    # 18048c90a starts bg_grace in non, which contains the opening eye.
    791: dict(archive="gmbg_kac5th_small", gauge_layer=0,
        initial=[layer("bg_all", "normal"), layer("bg_grace", "non")],
        events={event: [layer("bg_grace", label, slot=1)] for event, label in (("IN", "in"), ("OUT", "out"))}),
    826: standard_scene("superstarmitsuru", "bg_maxima", {"IN": 'in', "IN2": 'in2', "OUT": 'out'}),
    915: standard_scene("grace_vs_rasis", "bg_grace", {"IN": 'in', "OUT": 'out'}),
    **{song: standard_scene(archive, actor, {"IN": 'in', "OUT": 'out'}, upper=True)
       for song, archive, actor in ((1028,"kac6th_nianoa_s","bg_grace"), (1036,"kac6th_reigure_s","bg_grace"),
                                    (1099,"omega_nianoa","bg_grace"), (1100,"omega_hiyuki","bg_hiyuki"),
                                    (1269,"omega18_tama","bg_tama"), (1270,"omega18_maxma","bg_maxma"))},
    **{song: standard_scene(archive, actor, {"IN": 'in', "IN1": 'in1', "OUT": 'out'}, upper=True)
       for song, archive, actor in ((1361,"kac8th_kanade_s","bg_kanade"),
                (1462,"omega_inoten","bg_inoten"), (1495,"omega_capsaicin","bg_chara"))},
    **{song: standard_scene(archive, actor, {"IN": 'in', "IN1": 'in1', "OUT": 'out'},
                            upper=True, texture_packs=("gmbg_diver_tex",))
       for song, archive, actor in ((1584,"diver_02_rishna","bg_rishna"),
                                    (1660,"diver_03_noisya","bg_noisya"))},
    1148: dict(archive="gmbg_omega_maxma", gauge_layer=0, initial=[layer("bg_all","normal"), layer("bg_top",label="loop"), layer(), layer()],
        events={"IN": [layer("bg_maxma","in",slot=2)],
                **{f"IN{i}": [layer("bg_maxma",f"in{i}",slot=2)] for i in range(1,6)},
                "OUT": [layer("bg_maxma","out",slot=2)], "EX2_IN": [layer("bg_text","in",slot=3)],
                "EX2_IN1": [layer("bg_text","in1",slot=3)], "EX2_IN2": [layer("bg_text","in2",slot=3)],
                "EX2_OUT": [layer("bg_text","out",slot=3)], "EX2_OUT1": [layer("bg_text","out1",slot=3)]}),
    1362: dict(archive="gmbg_kac8th_lefrigh_s", gauge_layer=0, texture_packs=("gmbg_ver5test",),
        chart_timeline=dict(animation="bg_grace", slot=2, start="IN", intro="in",
            root_labels={"OUT": "out"}, labels={}, parameters={},
            # 180467279 sends the color cue to bg_grace/chara_usr as well as bg_red_blue.
            child_events={f"8THKAC_{event}": ("chara_usr", label) for event, label in (
                ("RED_IN", "red"), ("BLUE_IN", "blue"), ("RED_OUT", "red_out"),
                ("BLUE_OUT", "blue_out"), ("BLUE_RED", "blue_red"), ("RED_BLUE", "red_blue"))}),
        initial=[layer("bg_all","normal"),layer("bg_top",label=0),layer(),layer()],
        events={**{f"8THKAC_{color.upper()}_{action}": [layer("bg_red_blue",color if action=="IN" else color+"_out",slot=3)] for color in ("red","blue") for action in ("IN","OUT")},
                "8THKAC_BLUE_RED": [layer("bg_red_blue","blue_red",slot=3)],
                "8THKAC_RED_BLUE": [layer("bg_red_blue","red_blue",slot=3)]}),
    1811: dict(archive="gmbg_grace_challenge", gauge_layer=0, initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),layer()],
        events={"IN": [layer("bg_grace","in",slot=2)],"OUT": [layer("bg_grace","out",slot=2)],
                "EX2_IN1": [layer("bg_top","in1",slot=1)],"EX2_OUT": [layer("bg_top","out",slot=1)]}),
    1580: dict(archive="gmbg_kac9th_kureha", folder="666", gauge_layer=0, cutline_slot=3, texture_packs=("gmbg_diver_tex",),
        initial=[layer("bg_all", "normal"), layer("bg_top", "in"), layer(after_models=True)],
        events={**{event: [layer("bg_top", event.lower(), slot=1)] for event in ("IN", "IN1", "OUT")},
                **{f"EX2_{event}": [layer("bg_kureha_eff", event.lower(), slot=2)]
                   for event in ("IN", "IN1", "IN2", "IN3", "OUT")},
                **{f"EX3_{event}": [layer("cutline", event.lower(), slot=3)] for event in ("IN", "IN1")}}),
    1889: dict(archive="gmbg_diver_04_pilica", gauge_layer=0, texture_packs=("gmbg_diver_tex",), initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),layer(),layer(after_models=True)],
        events={**{event: [layer("bg_pilica", intro, slot=2)] for event,intro in (("IN","in"),("IN1","in1"),("IN2","in2"),("OUT","out"))},
                "EX2_IN": [layer("bg_effect","in",slot=3)]}),
    1888: dict(archive="gmbg_diver_05_haruka", gauge_layer=0, texture_packs=("gmbg_diver_tex",), initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),
                layer("bg_haruka_main",label="loop"),layer(),layer(after_models=True)],
        events={"HARUKA_"+label.upper(): [layer("bg_effect",label,slot=3),layer("bg_effect_on",label,slot=4)]
                for label in ("in","in1","in2")}),
    1902: dict(archive="gmbg_yambr_redbull", initial=[layer("bg_all","in"),layer("bg_ue","in",after_models=True)],
        events={"IN2": [layer(name,"in2",slot=i) for i,name in enumerate(("bg_all","bg_ue"))]},
        models=[dict(asset="rasis_redbull5g/rasis_redbull5g.model3.json",placement=[540,665,.75,.75])],
        motions={"LIVE2D_REDBULL5G_"+name: [intro,loop] for name,intro,loop in
                 (("IN","In","Idle"),("DRINK","Drink","Idle2"),("FLY","Fly","Idle3"),("OUT","Out",""))}),
    2037: dict(archive="gmbg_diver_06_shion", gauge_layer=0,
        texture_packs=("gmbg_diver_tex_blue",), initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),layer(),layer()],
        # 180497601–18049868e registers the nested labels and the six 0..100 tracks.
        chart_timeline=dict(animation="ar_main", slot=3, start="IN", intro="in",
            root_labels={"EX2_IN2": "in2", "EX2_IN3": "in3", "OUT": "out"},
            labels={f"x000200{i:02x}": target for i, target in enumerate(RAY_LABELS)},
            parameters={f"x000200{i:02x}": (path, "0", "100") for i, path in enumerate(RAY_PATHS[:6])}),
        events={"EX2_IN":[layer("background","in",slot=2)],
                "OUT":[layer("background","out",slot=2)]}),
    2038: dict(archive="gmbg_diver_07_nana", gauge_layer=0,
        texture_packs=("gmbg_diver_tex_blue", "gmbg_diver_tex_white"), chart_timelines=[dict(animation="bg_nana_out", slot=3, intro="non",
            root_labels={f"EX2_IN{i}": f"in{i}" for i in range(1, 6)},
            labels={"x00020000": ("bg_nana_usr", tuple(f"in{i}" for i in range(1, 6)))})],
        initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),layer(),layer(after_models=True),layer(after_models=True)],
        # bg_no 103 registers bg_nana_out, which already contains bg_nana_usr.
        # Starting bg_nana as well duplicates the character and its entrances.
        events={"OFF":[layer(slot=4)],"ON":[layer("bg_on_raindrop","in2",slot=4)],
                "IN2":[layer("bg_on_raindrop","in2",slot=4)],"IN5":[layer("bg_on_raindrop","in5",slot=4)]}),
    2087: dict(archive="gmbg_diver_soul", texture_packs=("gmbg_diver_tex_blue",),
        # 18049ab6a, 18049ac79 and 18049af33: basic02 priorities 66/65 feed
        # ColorConversion at 64. The SOUL movie at 61 draws over that result.
        gauge_layer=1,
        # The monitor AFPs are authored around x=824. The cabinet binds their
        # centers at x=540. Lower and upper monitors are independent children.
        initial=[layer("bg_top",0,options={"folder": "stage_bg/91"}, priority=66),
                 layer("bg_all","normal_loop",options={"folder": "stage_bg/91"}, priority=65),
                 # The unbound monitor and wave slots contain solid blue and red placeholders.
                 layer("bg_all",0,options={"hidden_instances": ["moni_usr", "wave_usr", "chara_usr"]}, priority=62),
                 layer(bounds=(-284,0,1648,1920), priority=62),layer(bounds=(-284,0,1648,1920), priority=62),
                 layer(priority=62),layer("bg_all",options={"root_instance": "chara_usr"}, priority=62),layer(foreground=True)],
        events={"SOUL_SPEED_HALF":[dict(slot=2,rate=.5)],"SOUL_SPEED_NORMAL":[dict(slot=2,rate=1)],
                "SOUL_INTRO":[layer("moni_in_ready","in",slot=3)],
                "SOUL_PHASE_A":[layer("wave_in_green","in",slot=5),
                                dict(slot=3,path="/",label="out")],
                "SOUL_PHASE_B":[layer("wave_in_red","in",slot=5),
                                layer("moni_in_red_bug","in_moni_ready",slot=3),
                                dict(slot=6,start=0,path="chara_usr",label="in_chara")],
                "SOUL_CHARA_OPEN":[dict(slot=6,path="chara_usr",label="chara_open")],
                "SOUL_BLUESCREEN":[dict(slot=2,path="bg_usr",label="in_bluescleen"),
                                   dict(slot=6,path="chara_usr",label="in_bluescleen"),
                                   layer("moni_in_bluescleen","in",slot=3),layer(slot=5)],
                "SOUL_REBOOT":[layer("moni_in_reboot","in",slot=4),layer("wave_in_reboot","in_reboot",slot=5)],
                "SOUL_WAVECHANGE":[dict(slot=5,path="/",label="in_wave_red"),
                                   dict(slot=2,path="bg_usr",label="in_reboot")],
                "SOUL_APPEAR":[dict(slot=6,path="chara_usr",label="in_reboot")],
                # 180467dc0 hides the lower monitor and cues the upper monitor
                # and the face without restarting either timeline.
                "SOUL_FINALATTACK":[layer(slot=3),dict(slot=4,path="/",label="in_eff_flash"),
                                    dict(slot=6,path="chara_usr/chara_usr/face_usr",label="in_egao")],
                "EX2_OFF":[layer(slot=7)], "EX2_IN":[layer("eff_flash","in",slot=7)],
                "EX2_OUT":[layer("eff_flash","out",slot=7)],
                "SOUL_ATTACK_OUT":[dict(slot=4,path="/",label="out_eff_flash"),
                                   dict(slot=6,path="chara_usr",label="out")]}),
    2152: dict(archive="gmbg_bpls3_final", chart_timelines=[dict(animation="bg_back_omm", slot=0, intro="in",
            root_labels={f"BASE_IN{i}": f"in{i}" for i in range(1, 10)},
            labels={"x00000000": (("monitor_screen_L_usr", "monitor_screen_R_usr"),
                                  tuple(f"in{i}" for i in range(1, 10)))})],
        initial=[layer("bg_back_omm",label="in"),layer("bg_bpls3_on_omm",label="in",after_models=True),
                 layer("bgef_top","in",after_models=True)],
        events={f"IN{i}":[layer("bg_bpls3_on_omm",f"in{i}",slot=1)] for i in range(1,10)},
        # Native draw priorities 63, 62, 61, 60, 59 retain their independent VOX slots.
        # RIGHT and LEFT share one MOC. The original motion handlers reflect LEFT on X.
        models=[dict(asset=f"{actor}_hexathlon_bpl/{actor}_hexathlon_game.model3.json",
                     placement=[540,762,scale,.88],slot=slot,priority=priority)
                for actor,scale,slot,priority in (("rasis",.88,0,63),("effect",.88,3,62),("left_right",.88,1,61),
                                                  ("left_right",-.88,2,60),("effect",.88,4,59))],
        motions={**{"LIVE2D_BPL3_RASIS_"+label.upper(): [label,loop] for label,loop in
                    (("In1","Idle1"),("Change1",""),("Change4","Idle5"),("Change5","Idle6"),("Change6","Idle7"),("Last1",""))},
                 "LIVE2D_BPL3_RASIS_LAST":["Last1",""],
                 **{"LIVE2D_BPL3_"+side+"_"+label.upper(): [label+suffix,loop] for side,suffix in (("RIGHT","a"),("LEFT","b"))
                    for label,loop in (("In1","Idle1"+suffix),("Change1","Idle2" if suffix=="a" else ""),
                        ("Change2","Idle3"+suffix),("Change3","Idle4" if suffix=="b" else ""),
                        ("Change5","Idle6"+suffix),("Change6","Idle7"+suffix))},
                 "LIVE2D_BPL3_LEFT_CHANGE4":["Change4",""],"LIVE2D_BPL3_RIGHT_LAST":["Last1a",""],"LIVE2D_BPL3_LEFT_LAST":["Last1b",""],
                 **{"LIVE2D_BPL3_EFFECT_"+name.upper():[name,""] for name in ("In1","In2","Last1","Last2")}}),
    2161: dict(archive="gmbg_bpls3_dream", chart_timelines=[dict(animation="bg_all", slot=0, intro="non", labels=label_tracks(0, (
            ("/", ("non", "in1", "in2", "in3", "out")),
            ("riot_usr/body_usr", ("in2", "in3", "out")), ("riot_usr/wing_usr", ("in2", "in3", "out")),
            ("moon_usr", ("non", "in1", "in2", "in3", "out")),
            ("riot_usr/wing_usr/blade_usr", ("stop", "loop")))))], initial=[layer()], events={}),
    # 18049b367 registers the common planet background behind bg_planet20.
    2198: dict(archive="gmbg_planet_yusha", texture_packs=("gmbg_planet",), gauge_layer=0,
        initial=[layer("bg_all", "normal_loop", priority=66, options={"folder": "stage_bg/110"}),
                 layer("bg_planet20", "in1")],
        events={"IN1": [layer("bg_planet20","in1",slot=1)],
                "IN2": [layer("bg_planet20","in2",slot=1)],"IN3": [layer("bg_planet20","in3",slot=1)]}),
    2199: dict(archive="gmbg_exgate_kannagi", extra_afp={"bg_all": "gmbg_exgate"}, chart_timelines=[dict(animation="bg_kannagi", slot=1, intro="in", labels=label_tracks(0, (
            ("usr_eff", numbered_labels(0, 5, 9)), ("usr_maingate", numbered_labels(0, 2)),
            (GATE_PATH, numbered_labels(0, 1, 3, 4, 5, 7, 9)), ("usr_sethu", numbered_labels(0, 6, 7, 8)))))],
        initial=[layer("bg_all", "in"), layer()], events={}),
    2253: dict(archive="gmbg_diver_09_rasisdeth", gauge_layer=0,
        texture_packs=("gmbg_diver_tex_purple",), chart_timelines=[dict(animation="bg_rasisdeth", slot=2, intro="in", start="IN",
            root_labels={f"IN{i}": f"in{i}" for i in range(2, 9)},
            labels={"x00010000": ("usr_out/usr_outwink", {8: "in8"})})],
        initial=[layer("bg_all","normal"),layer("bg_top",label="loop"),layer()],
        events={}),
    2330: dict(archive="gmbg_bpls5_final", chart_timelines=[
            dict(animation="bg_main", slot=1, intro="non", labels=label_tracks(0x20000, (
                ("/", ("in1", "in2", "in3", "in4", "in5", "in6", None, "in8", "in")),
                ("usr_hatch", {0: "in1", 8: "in"}), ("usr_hatch/usr_gate", {0: "in1", 8: "in"}),
                ("usr_flash_white", {1: "loop2"})))),
            dict(animation="bg_on", slot=2, intro="in1", start_label="x00010000:i0",
                labels=label_tracks(0x10000, (
                    ("/", ("in1", "in2", "in3", "in4", "in5", None, "in7", "in8")),
                    ("usr_drop1", {2: "in3"}), ("usr_drop2", {3: "in4"}), ("usr_drop3", {7: "in8"})))),
        ],
        # Priorities 10, 9 and 2. Model group 195 contains the body and effects (1805d7df8).
        # The noise pass at 18049df9e receives priority 66–69 (bg_under).
        initial=[layer(),layer("bg_main","non"),layer(after_models=True)],
        events={"x00030000:i4":[layer("bg_under","in5",slot=0)]},
        models=[dict(asset="konoha_kiskil/konoha_kiskil_game.model3.json",placement=[540,630,1.4,1.4],slot=0,
                     motions={f"LIVE2D_BPL5_EFFECT_{name.upper()}": [name, idle]
                              for name,idle in (("In","Idle"),("In2","Idle2"),("In3",""))}),
                dict(asset="konoha_kiskil_effect/konoha_kiskil_effect_game.model3.json",placement=[540,630,1.4,1.4],slot=0)],
        motions={"LIVE2D_BPL5_EFFECT_"+name.upper(): [name, ""] for name in ("In","In2","In3")}),
    # 18049e0b8 explicitly disables the base AFP for bg_no 118.
    2393: dict(archive="gmbg_exgate_alive", texture_packs=("gmbg_exgate",), chart_timelines=[dict(animation="bg_alive", slot=1, intro="in", labels=label_tracks(0x10000, (
            ("usr_eff", numbered_labels(0, 5, 9)), ("usr_maingate", numbered_labels(0, 2)),
            (GATE_PATH, numbered_labels(0, 1, 3, 4, 5, 7, 9)),
            (GATE_PATH + "/window_usr", numbered_labels(0, 2, 3)))))],
        # 18049e9c4 registers bg_alive_satan at 66, behind bg_alive at 65.
        # 18049ed90 captures only priorities 66–67 for the filter.
        initial=[layer(),layer("bg_alive","in")],
        events={f"x00020000:i{i}":[layer("bg_alive_satan",f"in{i}",slot=0)] for i in (6,7,8)}),
    2406: dict(archive="gmbg_vn26_game", texture_packs=("gmbg_vn26_final",), initial=[layer(),layer(),layer(after_models=True),layer(after_models=True)],
        events={**{f"x00030000:i{i}":[layer("bg_main",f"in{i-1}" if i>1 else "in",slot=0)]
                   for i in range(1,8)},
                **{f"x000{group}0000:i0":[layer(slot=slot)] for group,slot in ((1,2),(2,1),(3,0),(4,3))},
                "x00010000:i1":[layer("bg_on","in",slot=2)],
                "x00020000:i2":[layer("bg_center","in1",slot=1)],
                "x00040000:i1":[layer("bg_clea_all","in",slot=3)],
                "x00040000:i4":[layer("bg_clea_all","in3",slot=3)]},
        models=[dict(asset=f"{actor}_saihate/{actor}_saihate_game.model3.json",
                     placement=[540,450,.9,.9], priority=priority)
                for actor, priority in (("usagi",58),("pilica",59),("usagi",60))],
        motions={"LIVE2D_V2026_"+name: [intro,loop] for name,intro,loop in
                 (("PILICA_IN","In","Idle"),("PILICA_IN2","In2","Idle2"),("PILICA_LAST","In3",""),
                  ("PILICA_OUT","",""),("USAGI_IN","In2",""),("USAGI_LAST_FRONT","In3a",""),
                  ("USAGI_LAST_BACK","In3b",""),("USAGI_OUT_FRONT","",""),("USAGI_OUT_BACK","",""))}),
    # bg_no 119 registers the base and center light at 18049ef98 and 18049eff3.
    2413: dict(archive="gmbg_vn2026", gauge_layer=0, gauge_layers=[1], initial=[layer("bg_all", "normal"), layer("bg_center", "normal", foreground=True)], events={}),
    # 18049ff7d / 1804a008f register the center light and Maxima separately.
    2414: dict(archive="gmbg_vn2026", gauge_layer=0, gauge_layers=[2],
        extra_afp={"bg_vnmxm":"gmbg_vn2026_medley"}, initial=[layer("bg_all","normal"),layer(bounds=[-167,-354,1414,1920]),
                 layer("bg_center","normal",foreground=True)],
        events={"EX2_IN": [layer("bg_vnmxm",label="in",slot=1)]}),
    # 18048d2a3 registers the independently running Grace movie at priority 57.
    840: dict(archive="gmbg_304_aprilfool", initial=[layer("bg_all", "normal"), layer(foreground=True),
                 layer("grace", "normal_loop")],
        events={"IN": [layer("aprilfool_frame", "in", slot=1)],
                "OUT": [layer("aprilfool_frame", "out", slot=1)]},
        text=dict(slot="tex01", prefix="tex")),
    # 180490b84–180490cdc registers bg_top (priority 9) behind bg_rasis (priority 2).
    1185: dict(archive="gmbg_kac7th_rasis_s", initial=[layer("bg_all", label="normal"), layer(), layer(), layer(foreground=True)],
        events={"IN": [layer("bg_top", label="in", slot=1), layer("bg_rasis", label="in", slot=2)],
                "IN2": [layer("bg_top", label="in2", slot=1)],
                "OUT": [layer(slot=1), layer("bg_rasis", "out", slot=2)],
                "EX2_IN": [layer("bg_puchun", "in", slot=3)],
                "EX2_OUT": [layer("bg_puchun", label="out", slot=3)],
                "EX3_IN2": [layer("bg_rasis", label="in2", slot=2)]}),
    1219: dict(archive="gmbg_aprilfool2018", initial=[layer("bg_all", "normal"), layer("bg_top", 0), layer(), layer(), layer(foreground=True)],
        events={"IN": [layer("bg_maxma", "in", slot=2)],
                "IN1": [layer("bg_maxma", "in1", slot=2)],
                "IN2": [layer("bg_maxma", "in2", slot=2)],
                "OUT": [layer("bg_maxma", "out", slot=2)],
                **{event: [layer("bg_text", intro, slot=3),
                           layer("bg_text", intro, slot=4)]
                   for event, intro in (
                       ("EX2_IN", "in"), ("EX2_IN1", "in1"), ("EX2_IN2", "in2"),
                       ("EX2_IN3", "in3"), ("EX2_OUT", "out"), ("EX2_OUT1", "out1"))}}),
    # bg_no 76 registers ap_bg_all at 180491e9e. AutoparaBgView loads ap_monitor.
    1438: dict(archive="gmbg_automation_01", texture_packs=("gmbg_0501_stage",), monitor="ap_monitor", initial=[layer("ap_bg_all", "normal")], events={},
        gauge_layer=0),
    1751: dict(archive="gmbg_aprilfool2021", initial=[layer("bg_all", label="nomal"), layer(foreground=True)],
        events={"EXCEEDMASK_IN_WITHOUT_RASIS": [layer("text_flame", label="in", slot=1,
                                                     hide=["rasis_aplir_mini"])],
                "EXCEEDMASK_IN": [layer("text_flame", label="in", slot=1, show=["rasis_aplir_mini"])],
                "OUT": [layer("text_flame", "out", slot=1)]},
        text=dict(slot="text_10", prefix="text_")),
    # 1804949f1–180494c7a registers the XHRONOXAPSULΞ background and its sub-scenes.
    1766: dict(archive="gmbg_diver_xhrono", texture_packs=["gmbg_diver_tex", "gmbg_diver_tex_blue"], initial=[layer("bg_all", "normal"), layer("bg_top", "in"), layer()],
        tunnels=[None, ["star", 67.5, 2, 8]],
        events={**{f"IN{i}": [layer("bg_top", f"in{i}", slot=1)] for i in range(1, 8)},
                **{f"EX2_IN{i}": [layer("bg_1and2", f"in{i}", slot=2)]
                   for i in range(4, 7)},
                "OUT": [layer("bg_top", "out", slot=1), layer(slot=2)]},
        # 18046e2e0–18046eaa0 plays the gameplay models' Change, Idle and Meet groups.
        # 18046e29f and 18046ea5f change the group scale and anchor. The profile
        # at 1800671dc contributes a (20, -400) offset and a .65 scale factor.
        models=[dict(asset="mion_10thkac/mion_10thkac_0101.model3.json", placement=[560, 560, .65, .65], priority=57,
                     motion_placements={"LIVE2D_MION_IN": [560, 560, .65, .65],
                                        "LIVE2D_MION_IN2": [624, 502, .47125, .47125]}),
                dict(asset="rasis_10thkac/rasis_10thkac_0101.model3.json", placement=[490, 735, .98, .98], priority=58)],
        motions={"LIVE2D_MION_IN": ["In", "Idle"], "LIVE2D_MION_IN2": ["Meet_in", "Meet_idle"],
                 "LIVE2D_MION_OUT": ["Out1", ""], "LIVE2D_MION_OUT2": ["Meet_out", ""],
                 **{f"LIVE2D_MION_CHANGE{i}": [f"Change{i}", f"Idle{i}"] for i in range(1, 6)},
                 "LIVE2D_RASIS_I_IN": ["In", "Idle"], "LIVE2D_RASIS_I_OUT": ["Out", ""]}),
    # 1804953d6–180495a70 registers the background and the screen effects.
    1767: dict(archive="gmbg_diver_mixx", texture_packs=["gmbg_diver_tex", "gmbg_diver_tex_blue", "gmbg_diver_03_noisya"], initial=[layer("bg_all", "normal"), layer("bg_top", "in"),
                layer(), layer(), layer()],
        texture_slots={"mixxion": MIXXION_BG_SLOTS},
        # 180495760, 1804957d8–180495aa6. The named shapes sample the raw
        # capture and the outputs of CRT, chromatic aberration, and noise.
        frameLabels=[{name: layer("mixxion", name, slot=4)
                      for name in ("in", "in1", "in3", "in2")}],
        events={"IN1": [layer("bg_top", "in1", slot=1)],
                "IN2": [layer("bg_top", "in2", slot=1)],
                "OUT": [layer("bg_top", "out", slot=1)],
                "EX2_IN": [layer("bg_mixxion_eff", "in", slot=2)], "EX2_NON": [layer(slot=2)],
                "EX2_OUT": [layer("bg_mixxion_eff", "out", slot=2), layer(slot=3)],
                # VOX marks EX3_IN1 and EX3_IN2 as the two cutins.
                "EX3_IN1": [layer("cutin", "in1", slot=3)],
                "EX3_IN2": [layer("cutin", "in2", slot=3)]},
        models=[dict(asset="grace_mixxion_game/grace_mixxion_game.model3.json", placement=[540, 655, .75, .75])],
        motions={"LIVE2D_GRACE_IN": ["In", "Idle"], "LIVE2D_GRACE_GUN1": ["Gun1", "Idle2"],
                 "LIVE2D_GRACE_FLY": ["Fly", "Idle3"], "LIVE2D_GRACE_GUN2": ["Gun2", "Idle3"],
                 "LIVE2D_GRACE_FINAL": ["Final", ""]}),
    # 180496bce / 180496c26 / 180496ccd / 180496d74 registers the BPL finale layers.
    1938: dict(archive="gmbg_bpl_grace", texture_packs=["gmbg_diver_tex_blue", "gmbg_bpl_nomal"],
        chart_timeline=dict(animation="bg_all", slot=0, intro="in",
            child_events={"BPL2_MONITOR_IN": ("bg_bpl_usr", "in"),
                          "BPL2_MONITOR_GRACE_IN": ("bg_bpl_usr/monitor_usr", "in")}),
        initial=[layer("bg_all", label="in"), layer(), layer(after_models=True),
                layer("bg_all_on", label="in", after_models=True)],
        events={"IN": [layer("bg_grace_un", "in", slot=1)],
                "EX2_IN": [layer("bg_grace_on", "in", slot=2)],
                **{f"IN{i}": [layer("bg_grace_un", label=f"in{i}", slot=1)] for i in range(1, 5)},
                **{f"EX2_IN{i}": [layer("bg_grace_on", label=f"in{i}", slot=2)] for i in range(1, 5)},
                **{f"OUT{i}": [layer("bg_grace_un", f"out{i}", slot=1)] for i in range(1, 5)},
                **{f"EX2_OUT{i}": [layer("bg_grace_on", f"out{i}", slot=2)] for i in range(1, 5)}},
        # The game model registration at 180067969 uses a -210 Y offset and .64 scale.
        models=[dict(asset="grace_sudden_game/grace_sudden_game.model3.json", placement=[540, 750, .64, .64])],
        motions={"LIVE2D_BPL2_IN": ["In", "Idle1"], "LIVE2D_BPL2_1ST": ["EG1_in", "EG1"],
                 "LIVE2D_BPL2_1STEND": ["EG1_out", "Idle2"], "LIVE2D_BPL2_2ND": ["EG2_in", "EG2"],
                 "LIVE2D_BPL2_2NDEND": ["EG2_out", "Idle3"], "LIVE2D_BPL2_FULL": ["EG3_in", "EG3"],
                 "LIVE2D_BPL2_EX": ["EG4_in", ""], "LIVE2D_BPL2_OUT": ["EG4_out", ""]}),
    # 18049926d–1804999b3 registers Λkasha's background and lane inversions separately.
    2085: dict(archive="gmbg_diver_akasha", texture_packs=["gmbg_diver_tex_white"], initial=[layer("bg_top", label="loop"), layer(), layer(after_models=True),
                layer(after_models=True), layer(), layer()],
        events={**{f"BASE_IN{i}": [layer("bg_top", label=f"in{i}", slot=0)] for i in range(1, 5)},
                "BASE_OUT": [layer("bg_top", label="out", slot=0)],
                "EX2_IN3": [layer("bg_blind", label="in3", slot=1)],
                "EX2_IN4": [layer("bg_blind", label="in4", slot=1)],
                "IN5": [layer("bg_on", label="in5", slot=3)],
                "IN6": [layer("bg_on", label="in6", slot=3)],
                "IN7": [layer("bg_on", label="in7", slot=3)], "OUT": [layer("bg_on", label="out", slot=3)]},
        frameLabels=[{"in6": layer("bg_hanten", "in6", slot=4)},
                     {"in6": layer("bg_hanten", "in6", slot=5),
                      "in6b": layer("bg_hanten", "in6b", slot=5, path="/")}], inversion_layers=[4, 5],
        models=[dict(asset="ortlinde_akasha_game/ortlinde_akasha_game.model3.json", placement=[540, 685, .95, .95])],
        # EXHAUST uses the numeric forms at the same VOX positions as the named cues.
        motions={"LIVE2D_AKASHA_IN": ["In1", "Idle"], "LIVE2D_AKASHA_INVERTION": ["In2", "Idle2"],
                 "LIVE2D_AKASHA_LAST": ["Last", ""],
                 "i40": ["In1", "Idle"], "i41": ["In2", "Idle2"], "i42": ["Last", ""]}),
}


def original_playback(spec, source):
    events, parameters, movies = {}, {}, {}

    def register(slot, name):
        if (slot, name) not in movies:
            movie = movies[slot, name] = dict(slot=slot, name=name)
            if name == "bg_hanten":
                movie["texture_slots"] = ["nomal_usr", "gold_usr"]
            if slots := spec.get("texture_slots", {}).get(name):
                movie["texture_slots"] = slots
            if name == "bg_text" and spec["archive"] == "gmbg_aprilfool2018":
                movie["include" if spec["initial"][slot].get("foreground") else "exclude"] = MAXIMA_PROMPT_TEXTURES
        return movies[slot, name]

    for playback in spec.get("chart_timelines", [spec["chart_timeline"]] if "chart_timeline" in spec else []):
        slot, name = playback["slot"], playback["animation"]
        movie = register(slot, name)
        trigger = playback.get("start")
        start_label = playback.get("start_label")
        if trigger or start_label:
            key = f"EXBG:{trigger}" if trigger else f"EXMC_LABEL:{start_label}"
            events.setdefault(key, []).append(dict(slot=slot, name=name, label=playback["intro"], restart=True))
        else:
            movie["label"] = playback["intro"]
        for event, label in playback.get("root_labels", {}).items():
            events.setdefault(f"EXBG:{event}", []).append(dict(slot=slot, path="/", label=label))
        for event, (path, label) in playback.get("child_events", {}).items():
            events.setdefault(f"EXBG:{event}", []).append(dict(slot=slot, path=path, label=label))
        for address, (paths, labels) in playback.get("labels", {}).items():
            for index, label in (labels.items() if isinstance(labels, dict) else enumerate(labels)):
                if label:
                    events.setdefault(f"EXMC_LABEL:{address}:i{index}", []).extend(
                        dict(slot=slot, path=path, label=label) for path in ((paths,) if isinstance(paths, str) else paths)
                    )
        for address, (path, first, last) in playback.get("parameters", {}).items():
            parameters.setdefault(address, []).append(dict(slot=slot, path=path, **{"from": first, "to": last}))
    initial_slots = {slot for slot, _ in movies}
    for slot, entry in enumerate(spec["initial"]):
        if slot not in initial_slots and entry["animation"]:
            movie = register(slot, entry["animation"])
            movie.update(entry.get("options", {}))
            if entry["label"] != "":
                movie["label"] = entry["label"]

    def route(key, entry):
        slot = entry["slot"]
        action = dict(slot=slot)
        for field in ("hide", "show", "rate", "start"):
            if field in entry:
                action[field] = entry[field]
        if "path" in entry:
            action.update(path=entry["path"], label=entry["label"])
        elif "rate" not in entry:
            if name := entry["animation"]:
                register(slot, name)
                action.update(name=name, label=entry["label"], restart=True)
            else:
                action["stop"] = True
        events.setdefault(key, []).append(action)

    for event, entries in spec["events"].items():
        target = "EXMC_LABEL" if event.startswith("x") else "EXBG"
        for entry in entries:
            route(f"{target}:{event}", entry)
    for target, labels in enumerate(spec.get("frameLabels", ())):
        for label, entry in labels.items():
            route(f"SetFrameLabel:{target}:{label}", entry)

    manifest = dict(movies=list(movies.values()), events=events, parameters=parameters)
    gauges = []
    for slot in (*([spec["gauge_layer"]] if "gauge_layer" in spec else []), *spec.get("gauge_layers", ())):
        name = spec["initial"][slot]["animation"]
        movie = movies[slot, name]
        if "folder" in movie:
            # A shared background already carries its original gauge labels.
            gauge = dict(normal="normal", clear="clear")
        else:
            swf = import_module("compile-stage-background").BackgroundSWF(
                name, (source / "afp" / name).read_bytes(), (source / "afp/bsi" / name).read_bytes())
            swf.parse()
            gauge = import_module("prepare-stage-backgrounds").gauge_labels(swf)
            movie["label"] = gauge.pop("initial")
        gauges.append(dict(slot=slot, **gauge))
    if gauges:
        manifest["gauges"] = gauges
    if "monitor" in spec:
        name = spec["monitor"]
        slot = len(spec["initial"])
        monitor = register(slot, name)
        monitor["texture_aliases"] = {"ver5t_jak_40_i_eab": "monitor_jacket"}
        manifest["movies"].append(monitor)
        manifest["monitor"] = dict(slot=slot, name=name)
    if "cutline_slot" in spec:
        manifest["cutline_slot"] = spec["cutline_slot"]
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenes", type=Path)
    parser.add_argument("shared", type=Path, help="Shared original texture directory")
    parser.add_argument("output", type=Path)
    parser.add_argument("--songs", nargs="+", type=int, choices=SCENES, required=True)
    parser.add_argument("--live2d", type=Path, help="Original graphics/live2d directory for song models")
    parser.add_argument("--extra-archives", type=Path, action="append", default=[],
                        help="Additional extracted game_bg IFS directory")
    args = parser.parse_args()
    if args.live2d is None and any("models" in SCENES[song] for song in args.songs):
        parser.error("--live2d is required for songs with Cubism models")

    def package(name):
        for directory in (args.scenes, *args.extra_archives):
            for suffix in (".ifs", "_ifs"):
                path = directory / (name + suffix)
                if path.is_dir():
                    return path
        raise FileNotFoundError(f"Extracted AFP archive {name} was not found")

    prepare_scenes(package, args.shared, args.output, args.songs, args.live2d)


def prepare_scenes(package, shared, output, songs, live2d, publish=None):
    exporter = import_module("prepare-original-afp")
    publish = publish or exporter.copy_asset
    for song in songs:
        spec = SCENES[song]
        source = package(spec["archive"])
        destination = output / spec.get("folder", str(song)) / "background"
        afp = original_playback(spec, source)
        texture_dirs = (shared, *(package(name) / "tex" for name in spec.get("texture_packs", ())))
        extra_afp = {name: package(archive) for name, archive in spec.get("extra_afp", {}).items()}
        exporter.prepare(source, destination, afp, texture_dirs, extra_afp, publish)
        scene = {field: spec[field] for field in ("models", "motions", "tunnels",
                                                 "inversion_layers", "text") if field in spec}
        scene["initial"] = [{key: value for key, value in layer.items() if key in ("bounds", "after_models", "foreground", "priority")}
                            for layer in spec["initial"]]
        if "text" in scene:
            for file in (source / "tex").glob(scene["text"]["prefix"] + "[0-9][0-9].png"):
                publish(file, destination / file.name)
        for model in scene.get("models", ()):
            folder = Path(model["asset"]).parent
            shutil.copytree(live2d / folder, destination / "models" / folder, dirs_exist_ok=True)
        (destination / "scene.json").write_text(json.dumps(scene, separators=(",", ":")), encoding="utf-8")


if __name__ == "__main__":
    main()
