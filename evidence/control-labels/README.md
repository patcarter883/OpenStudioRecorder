# Evidence: the controls pane's labels

FLTK does not copy a label passed as `const char*` - `Fl_Widget::label(const char*)`
stores the **pointer** (only `copy_label()` strdups). The zoom commit (9c0e180) built
every widget in `btn_jxs_controls_cb`'s loop from a `char label[192]` scoped to the loop
body, so each widget's label pointed at stack that the next field's text overwrote.
Every row lost its name; the enum dropdowns kept theirs because `Fl_Menu_::add` strdups.

Both screenshots are the **same pane, same data, same host**, taken from a headless
`Xvfb :99` driven with `xdotool`. The recorder was pointed at a stub that serves the
`/api/controls` document captured from the live camera (`192.168.1.118:8080`,
`api-controls.json`) - byte-for-byte what the pane receives in the field, so all 26 rows
are the real ones (`AeExposureMode`, `AeConstraintMode`, `AwbMode`, `AfMode`, ... Zoom).

## before.png - `c42cbcf` (as deployed/at the time of the report)

* Every row name in the left column is **blank**.
* The status line at the top is **garbage** (`Ö^?`): that `Fl_Box` borrowed
  `ctrl.status` from the frame that had already returned.
* The window title is garbage too (`NoiseReductionModeMinimal`): it borrowed a
  temporary `std::string`'s buffer.
* The dropdown **values** are intact - `ExposureTimeModeAuto`, `ConstraintNormal`,
  `FlickerOff`, `AwbCustom`, `NoiseReductionModeOff`, `AfModeManual`, ... - which is
  exactly the reported symptom and the signature of the pointer, not the text, being lost.

## after.png - `fix/control-label-lifetime`

* All 26 rows carry their name, every widget kind (check box, dropdown, slider,
  value slider, text input).
* `AeEnable` / `AwbEnable` show their checkbox text.
* The Zoom row shows the zoom capability the zoom commit added:
  `Zoom  |  Digital zoom (crop, no lens movement): libcamera ScalerCrop`.

## Two extra defects the same screenshots exposed

1. **Sliders had never shown a name** - before or after the zoom work. `Fl_Slider::draw()`
   passes its own box to `draw_label()`, which for `FL_HOR_NICE_SLIDER` is the ~17px
   handle: the text is drawn into a space too small to read. The fix gives the slider
   rows an outside alignment (`FL_ALIGN_LEFT`), so `Fl_Group` draws the name in the left
   column with the rest. Proven in isolation by `slider_probe.cpp` (a two-slider window:
   default alignment invisible, `FL_ALIGN_LEFT` visible).
2. **The Zoom row's label is longer than the pane's ~177px name column**, so FLTK clips
   its left end (`Zoom  |  Digital zoom (crop, ...)`). The column width is a layout
   property of the pane, not a lifetime bug; it needs the widgets moved right or the pane
   widened, and is left for a follow-up rather than folded into a label-lifetime fix.

## The OBS plugin's pane does not have this bug

`open-broadcast/obs-jxs-camera/src/jxs-camera.c` also builds its rows from local buffers
(`char desc[JC_TEXT_MAX]`, `char line[128]`, `char label[...]`) and hands them to
`obs_properties_add_text/bool/list/int/float` and `obs_property_set_description`. libobs
duplicates every one of them: `new_prop()` does `p->desc = bstrdup(desc)`,
`obs_property_set_description()` does `bstrdup`, and `add_item()` (behind
`obs_property_list_add_*`) does `item.name = bstrdup(name)`. Nothing there borrows a
caller's buffer, so the plugin is not affected - the comment in the recorder's pane
("Same rule as the OBS plugin's pane") refers to the *labelling rule*, not the storage.
