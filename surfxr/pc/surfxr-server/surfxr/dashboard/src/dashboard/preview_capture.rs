//! Optional bounded capture of the offline preview through eframe's own renderer.
use eframe::egui::{self, Event, ViewportCommand};
use std::{
    io::Write,
    path::PathBuf,
    time::{Duration, Instant},
};

pub struct Capture {
    directory: PathBuf,
    pub page: usize,
    frames: usize,
    started: Instant,
}

impl Capture {
    pub fn from_args() -> Option<Self> {
        let args: Vec<_> = std::env::args_os().collect();
        let index = args.iter().position(|arg| arg == "--capture-dir")?;
        let directory = PathBuf::from(args.get(index + 1).expect("--capture-dir needs a path"));
        std::fs::create_dir_all(&directory).unwrap();
        Some(Self {
            directory,
            page: 0,
            frames: 0,
            started: Instant::now(),
        })
    }

    pub fn update(&mut self, ctx: &egui::Context) {
        if self.started.elapsed() > Duration::from_secs(45) {
            panic!("Offline preview capture timed out");
        }
        let image = ctx.input(|input| {
            input.events.iter().find_map(|event| {
                if let Event::Screenshot { image, .. } = event {
                    Some(image.clone())
                } else {
                    None
                }
            })
        });
        if let Some(image) = image {
            let path = self.directory.join(format!("settings-{}.ppm", self.page));
            let mut file = std::io::BufWriter::new(std::fs::File::create(path).unwrap());
            write!(file, "P6\n{} {}\n255\n", image.width(), image.height()).unwrap();
            for pixel in &image.pixels {
                file.write_all(&pixel.to_array()[..3]).unwrap();
            }
            file.flush().unwrap();
            self.page += 1;
            self.frames = 0;
            if self.page == 15 {
                ctx.send_viewport_cmd(ViewportCommand::Close);
            }
        }
        self.frames += 1;
        if self.frames == 5 && self.page < 15 {
            ctx.send_viewport_cmd(ViewportCommand::Screenshot(Default::default()));
        }
        ctx.request_repaint_after(Duration::from_millis(80));
    }
}
