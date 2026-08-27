use crate::binds::{fetch, Keybind};
use crate::watch::watch_config_reload;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::{Application, ApplicationWindow, CssProvider, Label, ListBox};
use gtk4_layer_shell::{Edge, Layer, LayerShell};

const APP_ID: &str = "dev.stellar.bindcheatsheet";

pub fn run() {
    let app = Application::builder().application_id(APP_ID).build();
    app.connect_activate(build_window);
    // No argv forwarding: we parse our own CLI flags in `main.rs`, not GTK's.
    app.run_with_args::<&str>(&[]);
}

fn build_window(app: &Application) {
    let provider = CssProvider::new();
    provider.load_from_data(include_str!("../style.css"));
    gtk4::style_context_add_provider_for_display(
        &gtk4::gdk::Display::default().expect("no display"),
        &provider,
        gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );

    let window = ApplicationWindow::builder()
        .application(app)
        .title("stellar")
        .build();

    window.init_layer_shell();
    window.set_layer(Layer::Overlay);
    window.set_anchor(Edge::Top, true);
    window.set_anchor(Edge::Bottom, true);
    window.set_anchor(Edge::Left, true);
    window.set_anchor(Edge::Right, true);
    window.set_exclusive_zone(-1);

    let list = ListBox::new();
    list.add_css_class("bind-list");
    render_binds(&list);
    window.set_child(Some(&list));
    window.present();

    // Cross-thread hand-off: `notify`'s watcher callback runs on its own
    // background thread, but GTK widgets aren't `Send`/thread-safe. A
    // `SendWeakRef` can cross threads (only `upgrade()`/deref back on the
    // owning thread), and `MainContext::invoke` marshals the actual
    // re-render back onto the main thread.
    let weak_list: glib::SendWeakRef<ListBox> = list.downgrade().into();
    let _ = watch_config_reload(move || {
        let weak_list = weak_list.clone();
        glib::MainContext::default().invoke(move || {
            if let Some(list) = weak_list.upgrade() {
                render_binds(&list);
            }
        });
    });
}

fn render_binds(list: &ListBox) {
    while let Some(row) = list.first_child() {
        list.remove(&row);
    }
    match fetch() {
        Ok(binds) => {
            for b in binds {
                list.append(&Label::new(Some(&format_bind(&b))));
            }
        }
        Err(e) => list.append(&Label::new(Some(&format!("stellar: {e}")))),
    }
}

fn format_bind(b: &Keybind) -> String {
    if b.submap.is_empty() {
        format!("{} {} -> {} {}", b.modmask, b.key, b.dispatcher, b.arg)
    } else {
        format!(
            "[{}] {} {} -> {} {}",
            b.submap, b.modmask, b.key, b.dispatcher, b.arg
        )
    }
}
