# Asset path baseline

Expected staged asset paths from the source-only HDD conversion (2026101300 binary), using the `stageNablaAssets` rules for version 2026101300. Each digest covers the sorted relative paths in its group as UTF-8 text with forward slashes and one LF after each path.

## Verify an asset directory or APK

Run from the repository root with Python 3.13. Passing the staged assets directory or an APK is sufficient. The tool reads the table below automatically.

```powershell
python .\tools\check-asset-paths.py --merged 'D:\path\to\nabla-assets'
python .\tools\check-asset-paths.py --apk 'D:\path\to\app.apk'
```

Baseline verification checks the exact path sets of groups other than `music` (original HDD folders) and `songs` (generated catalog and presentations). Song paths vary with the local export. The tool reports the raw music group comparison for information and its differences do not affect the exit status. For checked groups, extra paths, missing paths, and renames are failures, including changes that leave the group count unchanged. Diagnostics list affected groups and counts. APK verification reads the ZIP directory under `assets/`.

If you have matching reference and override roots, supply both to check that the output contains every expected path while allowing additional paths.

```powershell
python .\tools\check-asset-paths.py --reference 'D:\path\to\reference' --override 'D:\path\to\override' --merged 'D:\path\to\nabla-assets'
```

This mode writes missing path names to `local-inputs/asset-paths-missing.txt`. Omit the comparison target to print the expected group counts and digests for the two roots. Both verification modes return status 1 for a mismatch.

The earlier development tree retained superseded debug layout tables, unused copies of AFP textures and bank metadata. The current exporters index the original background IFS instead of storing decoded per-frame PNGs. This baseline describes those physical files. It contains no game data or filenames below the top-level directories.

## Expected paths

| Asset directory | Required paths | Path SHA-256 |
| --- | ---: | --- |
| beam | 38 | `6d36fc46a4c4279f818ce82e570038e95a0dc8376c56f1d81f3dc20edc261b76` |
| button_hit | 432 | `2877498049f9213bd7edf9ee5eef068be44075ff2c18abdb962d021cdfa4df46` |
| cabinet | 45 | `97a5a0576b21fc3024a6d5eb15ba5295459abb0fafbde97828c06a8e4f819af7` |
| cabinet_timer | 45 | `a264775d3fe21ae91c58c51e1898cd5c5e96fd19c3c1ea5f394c0f842019a0bc` |
| caution_original | 39 | `c7ba9395cec0a465c9d3504fa6d6d8563a4161908fc2379f43983bc84168e984` |
| crew | 898 | `cdf53fd6563ed03d39f823671c7c9c1a2bf8a17b0387a2a62a65ce5052cb2bd0` |
| crew_selector | 217 | `7fc581f4be7ea37bce6cea05ab68c3222465724fbbc3eb920ae97139357a8c56` |
| entry_checkin | 37 | `117e578314ba67aad585eaf682bc454135eea0ccadfa6ad7b74fa5e1d477b16d` |
| entry_flow | 84 | `3d74d3837b31eb9843c831a9d8fc92622252fff624aece4c491c0dbd0e587d56` |
| entry_original | 294 | `f09436e9be3c57ab0b7dea608c424c1466a499909bb11a83c311ea42845df2f6` |
| entry_profile | 33 | `361cdc8d447e1ccd6195c555f039256edb5701d9366e66f33083e81f9a16722a` |
| entry_wait | 13 | `7cc6807164bf9b35cd4dbea3edc8e4ff8983a92a43c5f3f387f23dc3ac5c12af` |
| ex_track | 18 | `450227cd11fe37e2a1f44b9b2c067fd2cce787bf3c5bbb054496bb49aae8e75a` |
| flow | 12 | `480e315ab85a160f710f773ec3ec94effc99db98678b487177c8c6609f480466` |
| font | 7 | `fa0ec3cbb2f9d36111e23342cded2fd56625b595216198fcb7754cb28f03f085` |
| game_nemsys | 111 | `41189c9b85e0cf2aadf30a1219ace3788e9e06bd3df7b717b425d9a7fb1bcd6c` |
| generator_vii | 214 | `2a799e2d44f9fa32dbfcf3bcfca23b2b8a240d5a0cbbeb181b68d7911b24131d` |
| graphics | 104 | `0756e3bafbc26f4cd124c6c97973e2895b4f64b2bab52ca8d3c51ef63c7b0e50` |
| hud | 286 | `860c0fdac49a7718c1daaba08fd9de664b9de5dba38d831b855b4362221d3a05` |
| hud_chain | 25 | `7ccf926ef0051a0d1a1ce5003feb478df1c416cebc44ea2764ff20d638e50410` |
| laser_effect | 1093 | `a6381ae7a07654da1872470c5feac2349ec3a70e9896273406f5659ba5d7a085` |
| laser_notice | 18 | `122a0c5577ad83f79c401e2b9539a35ae01a4bb946b9fa4f192db30c29336561` |
| matching | 85 | `7adba09136008e8ec95cd2d6141404539712d528895207db590328b03de2cad0` |
| menu | 406 | `1daeb30fa13d5e4a61f5c387bcc40a71dd85bba731dcd3356589fba205b06392` |
| mode_flow | 65 | `b0f41a16fa097e46782016cd200bb3206e3b0121b6cf03c8b353dd7a36b1882d` |
| music | 840 | `6c9d7de8f6d2c7b88c97a6fc488c0cecfafca497e766f656b3b75d618398c2c4` |
| next_area | 94 | `8492ebd0e10f9e0bbe1591c0a37a337f17b4bbe71ad9725087b25f4b0610c17b` |
| payment | 131 | `9113cf85206fe2badb1b59ced86740fb59fd0648aef92e1d930ea7b55d8ba129` |
| player_card | 48 | `95750c12ce31cc6b65463041d725af6cd03b88973d254ab57b83b153275d81d4` |
| result | 2 | `f54df8bd8d3419a963f7900bad7606960b139331b5fa00cb891c0cfb13299a4b` |
| result_details | 37 | `da065199e6c77eec99531f1f5b6934b6969a4f2fb4a3860da6a983718c119bd0` |
| result_force | 203 | `44d3dd7a92e224212aa7677179edf01c7fa8e1725b61d0501f21293229d8cd40` |
| result_gauge | 48 | `043bca6b306a7febb336a90e530fb6cbc19dbdd7707f41df2898827af759ec8c` |
| result_grade | 151 | `b3072670d680314b1bf31b32138c70981f139df6af7e1b65e95cfe4e66322456` |
| result_original | 190 | `8f55814f45c89c32333f06645e8994bcd1583bee53355927349971c51e37549a` |
| result_profile | 44 | `a639d681b1920c2ffe106674d931e43e6c4f35ac8e343225540c42eab57b52dd` |
| result_score | 51 | `aa9e2945455c8084067b414f5c29b039bad0f19c7ecdf9220d513e274cd38f90` |
| room_flow | 63 | `ec474262b93c1bb32aac8174b619aa5e3ee2321143ebdb50a139b59027e14415` |
| selection_ui | 162 | `957c39e3dc0fdc5b65e70c50f6937a90a61d987eac3a2ee78a7b5c6bd7192476` |
| session_dialog | 38 | `5192b02264fba5d67cf385c9f94763ef2080a4e1070c1d57604fb8d610812947` |
| settings | 477 | `97ce9eeeac793018b91b6819c65b00e80449823fd5e5bc79521b838327f45d0f` |
| shutter_nabla | 53 | `6420b0a45fa66772574b30ad7c76ecf0cb8c5c0880abeb7ba758f5f39093e7b2` |
| skill_effects | 202 | `0eee32fa3a1ee97af9a201a4dfe4a600d660f91a3e02865c3bd2fc69c6039f0c` |
| skill_selector | 112 | `ee828b3e7759196423064a35ee65b65e5ffd6987e473e9f7b1f09687ad90d34a` |
| song_row | 129 | `005894367886a5a7690982fc0ba08f5b245a752efe374ca1ff12b9f0b6b09001` |
| songs | 594 | `aca76de873222ec5cdc9f0f5d52520f36f89405e7f0d25ad8e51f7ddbe4093ca` |
| stage | 62 | `71faa94e16b4f6210ed729943ba40f8588ca099102851401a9c74ecc2823cd76` |
| stage_bg | 93 | `dde2f57546bda60608557255273cf8a96231dc694b1e9a23050547468be37096` |
| stage_buttons | 400 | `83b4658d9656ec81105a96dfc703d238875c723d9f2a7bfe26dff932578a18e3` |
| stage_end | 215 | `b8570ee06fffa06722128d82f545a39db6e6e857b0882030eac76df7813f323e` |
| stage_ready | 33 | `64d9b97cbbdb973a96074da0381713c7176b7111a857c18d8a4f0e2381c78a0b` |
| startup | 4 | `33aead2f62efd7085d3b1247efa2262f7c77975972e8e166eb063215d4b64749` |
| subscreen | 4094 | `972ffdd69a5b98a1585413621a43fbd00a27d35b343bafbc619b02148fb5003f` |
| title_original | 87 | `bc4f3c31b29956f159328fb77f5113438401f74da42c69a7e9d52a57276021ef` |
| total_result | 124 | `c9e7dae884410a416ea4531c1f3874028ee873342520e7d5d25d322f34ef8dbe` |
| track_entry | 99 | `b8e4a7055c1f47cf72312d766507237708c8c3c0ce2752cf478c79e7e2791a20` |
| track_header | 20 | `fd2eea0e92178624f3ea36bc1998c68e8338a05d975bf62b5da93b80b1c68961` |
| track_intro | 115 | `2b7c7ef07ed12d5df9bd9a4710e80024ae005496baef7a77279940bf605372ed` |
| ui | 17 | `e8b42617249df34f13438f1e9af41623d0fb68d18745bb17df5dc0492865248f` |
| valkyrie | 22 | `24700c235ac09e42581649e110561138cf57eab5f8233fd3c720053a5bf0b199` |
