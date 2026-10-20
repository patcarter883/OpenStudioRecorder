// What does FLTK 1.3 do with a labelled slider? Build and screenshot.
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Check_Button.H>
#include <string>

int main()
{
  Fl_Window w(520, 220, "slider label probe");

  Fl_Check_Button* cb = new Fl_Check_Button(25, 10, 300, 25, "AeEnable");
  cb->copy_label("AeEnable");

  Fl_Slider* a = new Fl_Slider(180, 40, 250, 25, "ExposureTime");
  a->type(FL_HOR_NICE_SLIDER);
  a->bounds(0, 100);
  a->value(40);
  a->copy_label("ExposureTime");

  Fl_Value_Slider* b = new Fl_Value_Slider(180, 70, 250, 25, "AnalogueGain");
  b->type(FL_HOR_NICE_SLIDER);
  b->bounds(0, 16);
  b->value(3.95);
  b->copy_label("AnalogueGain");

  // same widget, label placed outside the slider's own box
  Fl_Slider* c = new Fl_Slider(180, 100, 250, 25, "Brightness");
  c->type(FL_HOR_NICE_SLIDER);
  c->bounds(0, 100);
  c->value(50);
  c->copy_label("Brightness");
  c->align(FL_ALIGN_LEFT);

  // a plain Fl_Slider in the default FL_SLIDER type, for comparison
  Fl_Slider* d = new Fl_Slider(180, 130, 250, 25, "Contrast");
  d->bounds(0, 100);
  d->value(50);
  d->copy_label("Contrast");

  w.end();
  w.show();
  return Fl::run();
}
