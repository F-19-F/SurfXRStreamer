use super::{NestingInfo, reset};
use crate::dashboard::components::f64_eq;
use surfxr_gui_common::theme::SCROLLBAR_DOT_DIAMETER;
use surfxr_packets::PathValuePair;
use surfxr_session::settings_schema::{NumberType, NumericGuiType};
use eframe::{
    egui::{DragValue, Layout, Slider, Ui},
    emath::Align,
};
use json::Number;
use serde_json as json;

fn to_json_value(number: f64, ty: NumberType) -> json::Value {
    match ty {
        NumberType::UnsignedInteger => json::Value::from(number.abs() as u64),
        NumberType::SignedInteger => json::Value::from(number as i64),
        NumberType::Float => json::Value::Number(Number::from_f64(number).unwrap()),
    }
}

pub struct Control {
    nesting_info: NestingInfo,
    editing_value_f64: Option<f64>,
    default: f64,
    default_string: String,
    ty: NumberType,
    gui_type: NumericGuiType,
    suffix: Option<String>,
}

impl Control {
    pub fn new(
        nesting_info: NestingInfo,
        default: f64,
        ty: NumberType,
        gui: NumericGuiType,
        suffix: Option<String>,
    ) -> Self {
        let default_string = format!("{default}{}", suffix.clone().unwrap_or_default());

        Self {
            nesting_info,
            editing_value_f64: None,
            default,
            default_string,
            ty,
            gui_type: gui,
            suffix,
        }
    }

    pub fn ui(
        &mut self,
        ui: &mut Ui,
        session_fragment: &mut json::Value,
        allow_inline: bool,
    ) -> Option<PathValuePair> {
        super::grid_flow_inline(ui, allow_inline);

        let mut session_value = session_fragment.as_f64().unwrap();
        let saved_value = session_value;

        let mut request = None;

        fn get_request(
            nesting_info: &NestingInfo,
            number: f64,
            ty: NumberType,
        ) -> Option<PathValuePair> {
            Some(PathValuePair {
                path: nesting_info.path.clone(),
                value: to_json_value(number, ty),
            })
        }

        ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
            let editing_value_mut = if let Some(editing_value_mut) = &mut self.editing_value_f64 {
                editing_value_mut
            } else {
                &mut session_value
            };

            let (response, gained_focus, lost_focus) = match &self.gui_type {
                NumericGuiType::Slider {
                    range,
                    step,
                    logarithmic,
                } => {
                    let slider_width = (ui.available_width() - 200.0).max(64.0);
                    ui.spacing_mut().slider_width = ui.spacing().slider_width.min(slider_width);
                    let mut slider = Slider::new(editing_value_mut, range.clone())
                        .logarithmic(*logarithmic)
                        .show_value(false);
                    if let Some(step) = step {
                        slider = slider.step_by(*step);
                    }
                    if !matches!(self.ty, NumberType::Float) {
                        slider = slider.integer();
                    }
                    let slider_response = {
                        ui.style_mut().spacing.interact_size.y = SCROLLBAR_DOT_DIAMETER;
                        ui.add(slider)
                    };

                    let mut drag_value = DragValue::new(editing_value_mut);
                    // Note: the following ifs cannot be merged with the ones above to avoid double
                    // mutable borrow of editing_value_mut.
                    if let Some(step) = step {
                        drag_value = drag_value.speed(*step);
                    }
                    if !matches!(self.ty, NumberType::Float) {
                        drag_value = drag_value.fixed_decimals(0);
                    }
                    if let Some(suffix) = &self.suffix {
                        drag_value = drag_value.suffix(suffix);
                    }
                    let textbox_response = ui.add(drag_value);

                    // Response::union keeps the slider's ID. Focus belongs to
                    // the text editor's ID, so read it before combining hit boxes.
                    let gained = slider_response.gained_focus() || textbox_response.gained_focus();
                    let lost = slider_response.lost_focus() || textbox_response.lost_focus();
                    (slider_response.union(textbox_response), gained, lost)
                }
                NumericGuiType::TextBox => {
                    let mut drag_value = DragValue::new(editing_value_mut);

                    if !matches!(self.ty, NumberType::Float) {
                        drag_value = drag_value.fixed_decimals(0);
                    }
                    if let Some(suffix) = &self.suffix {
                        drag_value = drag_value.suffix(suffix);
                    }

                    let response=ui.add(drag_value);
                    let gained=response.gained_focus();
                    let lost=response.lost_focus();
                    (response,gained,lost)
                }
            };

            if response.drag_stopped() || lost_focus {
                if !f64_eq(*editing_value_mut, saved_value) {
                    request = get_request(&self.nesting_info, *editing_value_mut, self.ty);
                    *session_fragment = to_json_value(*editing_value_mut, self.ty);
                }

                self.editing_value_f64 = None;
            } else if response.drag_started() || gained_focus || response.clicked() {
                self.editing_value_f64 = Some(session_value)
            }

            if reset::reset_button(
                ui,
                !f64_eq(session_value, self.default),
                &self.default_string,
            )
            .clicked()
            {
                request = get_request(&self.nesting_info, self.default, self.ty);
            }
        });

        request
    }
}
