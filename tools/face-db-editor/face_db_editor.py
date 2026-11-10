"""A small editor for the face database of objective-control.

The application writes the enrolled faces into a PostgreSQL table (or into
a flat file, but this editor manages the PostgreSQL one). What it stores
about a person -- the name that is shown on screen, the picture of the face
and an arbitrary JSON document -- is written once, when the person is
enrolled, and is rarely right afterwards. A name gets misspelled, a piece
of metadata turns out to be missing, the wrong picture was captured.

This editor opens that table and lets the properties be looked at and
corrected:

  * the people who are enrolled, with their id and how many samples each
    of them has,
  * the samples of the selected person, with the picture of the face that
    was captured at the time,
  * the name and the metadata of the person, which can be edited and
    stored again,
  * the picture of a face, which can be saved to a file.

The properties are stored with *every* sample of a person, so that the
store can answer "who is this?" without a second lookup. The editor keeps
that promise: a change to the name or to the metadata is written to all
the samples of the person, exactly as the C++ store writes them when it
enrols somebody.

Run it with::

    python3 face_db_editor.py --dsn=postgres://user@localhost/objective_control

The connection string is taken from ``--dsn``, else from ``$FACE_DB_URL``,
else from ``$DATABASE_URL`` -- the same order the C++ application uses.
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Any

try:
    from face_db import FaceDbError, FaceStore, format_metadata, parse_metadata
except ImportError:  # pragma: no cover - the module sits next to this file
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from face_db import FaceDbError, FaceStore, format_metadata, parse_metadata


#: How large the preview of a face is, in pixels.
PREVIEW_SIZE = 220

#: The environment variables the connection string is taken from, in the
#: order the C++ application looks them up in.
DSN_ENV_VARS = ("FACE_DB_URL", "DATABASE_URL")


def sniff_image_format(data: bytes) -> str:
    """Returns the format of an encoded image: "jpeg", "png" or "unknown".

    The picture of a face is stored as the bytes of an encoded image, and
    the C++ application writes a JPEG. The editor has to know, because
    Tk can only show a PNG and because the extension of the file the user
    saves should match what is in it.
    """
    if data.startswith(b"\xff\xd8\xff"):
        return "jpeg"
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "png"
    return "unknown"


def image_to_png(data: bytes) -> bytes | None:
    """Converts an encoded image to PNG, or returns None.

    Tk can display a PNG but not a JPEG, so the picture has to be
    converted before it can be shown. Pillow does that when it is
    installed; otherwise ImageMagick is asked to do it. When neither is
    there the picture cannot be shown, but it can still be saved -- that
    needs no conversion at all.
    """
    try:
        from PIL import Image
    except ImportError:
        Image = None  # type: ignore[assignment]

    if Image is not None:
        try:
            with Image.open(io.BytesIO(data)) as image:
                image = image.convert("RGB")
                # The preview is a fixed size, so the picture is scaled
                # down to fit it. A small picture is left alone.
                image.thumbnail((PREVIEW_SIZE, PREVIEW_SIZE))
                buffer = io.BytesIO()
                image.save(buffer, format="PNG")
                return buffer.getvalue()
        except Exception:
            return None

    # ImageMagick reads from stdin and writes to stdout. The explicit
    # "-" is what says so; "magick png:-" alone is not a valid command.
    for executable in ("magick", "convert"):
        path = shutil.which(executable)
        if path is None:
            continue
        try:
            finished = subprocess.run(
                [path, "-", "png:-"],
                input=data,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=15,
                check=False,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if finished.returncode == 0 and finished.stdout:
            return finished.stdout
    return None


class FaceDbEditor:
    """The editor window.

    The class holds the widgets and the store; it does not inherit from
    ``tk.Tk`` so that it can be driven from a test with a window of its
    own.
    """

    def __init__(self, root: tk.Tk, dsn: str, table: str) -> None:
        self.root = root
        self.store = FaceStore(dsn, table)
        self._photo_image: tk.PhotoImage | None = None
        self._photo_data: bytes | None = None
        self._photo_sample_id: int | None = None

        root.title("Face database editor")
        root.geometry("1120x720")
        root.minsize(900, 560)

        self._build_widgets()
        self._show_connection_state()
        self.connect()

    # -- widgets ------------------------------------------------------------

    def _build_widgets(self) -> None:
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")

        outer = ttk.Frame(self.root, padding=8)
        outer.grid(row=0, column=0, sticky="nsew")
        self.root.rowconfigure(0, weight=1)
        self.root.columnconfigure(0, weight=1)
        outer.rowconfigure(2, weight=1)
        outer.columnconfigure(0, weight=1)

        self._build_connection_bar(outer)
        self._build_warning(outer)
        self._build_body(outer)
        self._build_status_bar(outer)

    def _build_connection_bar(self, parent: ttk.Frame) -> None:
        bar = ttk.Frame(parent)
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.columnconfigure(1, weight=1)

        ttk.Label(bar, text="Database:").grid(row=0, column=0, sticky="w")
        self.dsn_var = tk.StringVar(value=self.store.dsn)
        self.dsn_entry = ttk.Entry(bar, textvariable=self.dsn_var)
        self.dsn_entry.grid(row=0, column=1, sticky="ew", padx=6)

        ttk.Label(bar, text="Table:").grid(row=0, column=2, sticky="w")
        self.table_var = tk.StringVar(value=self.store.table)
        self.table_entry = ttk.Entry(bar, textvariable=self.table_var, width=20)
        self.table_entry.grid(row=0, column=3, sticky="w", padx=6)

        self.connect_button = ttk.Button(bar, text="Connect", command=self.connect)
        self.connect_button.grid(row=0, column=4, sticky="e")

        self.root.bind("<Return>", lambda _event: self.connect())

    def _build_warning(self, parent: ttk.Frame) -> None:
        self.warning_var = tk.StringVar()
        self.warning_label = ttk.Label(
            parent,
            textvariable=self.warning_var,
            foreground="#b00020",
            wraplength=1000,
            justify="left",
        )
        self.warning_label.grid(row=1, column=0, sticky="ew", pady=(0, 6))
        self.warning_label.grid_remove()

    def _build_body(self, parent: ttk.Frame) -> None:
        pane = ttk.PanedWindow(parent, orient="horizontal")
        pane.grid(row=2, column=0, sticky="nsew")

        pane.add(self._build_people_pane(pane), weight=1)
        pane.add(self._build_detail_pane(pane), weight=3)

    def _build_people_pane(self, parent: ttk.Frame) -> ttk.Frame:
        frame = ttk.Labelframe(parent, text="People", padding=6)
        frame.rowconfigure(1, weight=1)
        frame.columnconfigure(0, weight=1)

        self.filter_var = tk.StringVar()
        filter_entry = ttk.Entry(frame, textvariable=self.filter_var)
        filter_entry.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 6))
        filter_entry.insert(0, "")
        self.filter_var.trace_add("write", lambda *_: self._refresh_people())

        columns = ("id", "name", "samples")
        self.people_tree = ttk.Treeview(frame, columns=columns, show="headings", selectmode="browse")
        self.people_tree.heading("id", text="Id")
        self.people_tree.heading("name", text="Name")
        self.people_tree.heading("samples", text="Samples")
        self.people_tree.column("id", width=60, anchor="e", stretch=False)
        self.people_tree.column("name", width=180, stretch=True)
        self.people_tree.column("samples", width=70, anchor="e", stretch=False)
        self.people_tree.grid(row=1, column=0, sticky="nsew")

        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=self.people_tree.yview)
        scrollbar.grid(row=1, column=1, sticky="ns")
        self.people_tree.configure(yscrollcommand=scrollbar.set)
        self.people_tree.bind("<<TreeviewSelect>>", self._on_person_selected)

        return frame

    def _build_detail_pane(self, parent: ttk.Frame) -> ttk.Frame:
        frame = ttk.Frame(parent)
        frame.rowconfigure(1, weight=1)
        frame.columnconfigure(0, weight=1)

        samples_frame = ttk.Labelframe(frame, text="Samples", padding=6)
        samples_frame.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        samples_frame.columnconfigure(0, weight=1)

        columns = ("id", "enrolled", "photo")
        self.samples_tree = ttk.Treeview(
            samples_frame, columns=columns, show="headings", selectmode="browse", height=6
        )
        self.samples_tree.heading("id", text="Id")
        self.samples_tree.heading("enrolled", text="Enrolled")
        self.samples_tree.heading("photo", text="Picture")
        self.samples_tree.column("id", width=60, anchor="e", stretch=False)
        self.samples_tree.column("enrolled", width=200, stretch=True)
        self.samples_tree.column("photo", width=80, anchor="center", stretch=False)
        self.samples_tree.grid(row=0, column=0, sticky="ew")

        scrollbar = ttk.Scrollbar(samples_frame, orient="vertical", command=self.samples_tree.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.samples_tree.configure(yscrollcommand=scrollbar.set)
        self.samples_tree.bind("<<TreeviewSelect>>", self._on_sample_selected)

        lower = ttk.Frame(frame)
        lower.grid(row=1, column=0, sticky="nsew")
        lower.columnconfigure(1, weight=1)
        lower.rowconfigure(0, weight=1)

        self._build_photo_pane(lower)
        self._build_properties_pane(lower)

        return frame

    def _build_photo_pane(self, parent: ttk.Frame) -> None:
        frame = ttk.Labelframe(parent, text="Face", padding=6)
        frame.grid(row=0, column=0, sticky="nsew", padx=(0, 6))

        # The pane keeps its size whatever the picture is, so that the
        # window does not jump around when a person is selected.
        frame.configure(width=PREVIEW_SIZE + 12, height=PREVIEW_SIZE + 12)
        frame.grid_propagate(False)

        # The label is given no width and no height of its own: those
        # options of a Tk label are counted in characters and in lines,
        # not in pixels, and they would clip the picture down to a
        # sliver. Without them the label takes the size of the picture.
        self.photo_label = tk.Label(
            frame,
            text="no picture",
            relief="sunken",
            borderwidth=1,
            anchor="center",
        )
        self.photo_label.pack(fill="both", expand=True)

        self.save_photo_button = ttk.Button(
            frame, text="Save picture as...", command=self.save_photo, state="disabled"
        )
        self.save_photo_button.pack(pady=(6, 0))

    def _build_properties_pane(self, parent: ttk.Frame) -> None:
        frame = ttk.Labelframe(parent, text="Properties", padding=6)
        frame.grid(row=0, column=1, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(2, weight=1)

        ttk.Label(frame, text="Name:").grid(row=0, column=0, sticky="w")
        self.name_var = tk.StringVar()
        self.name_entry = ttk.Entry(frame, textvariable=self.name_var)
        self.name_entry.grid(row=0, column=1, sticky="ew", padx=6, pady=(0, 6))

        ttk.Label(frame, text="Metadata:").grid(row=1, column=0, sticky="nw")
        self.metadata_text = tk.Text(frame, height=10, wrap="none", undo=True)
        self.metadata_text.grid(row=1, column=1, rowspan=2, sticky="nsew", padx=6)
        metadata_scroll = ttk.Scrollbar(frame, orient="vertical", command=self.metadata_text.yview)
        metadata_scroll.grid(row=1, column=2, rowspan=2, sticky="ns")
        self.metadata_text.configure(yscrollcommand=metadata_scroll.set)

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(8, 0))

        self.save_button = ttk.Button(buttons, text="Save changes", command=self.save_changes)
        self.save_button.pack(side="left")

        self.format_button = ttk.Button(buttons, text="Format JSON", command=self.format_metadata)
        self.format_button.pack(side="left", padx=6)

        self.refresh_button = ttk.Button(buttons, text="Refresh", command=self.refresh)
        self.refresh_button.pack(side="left")

        self.delete_button = ttk.Button(buttons, text="Delete person", command=self.delete_person)
        self.delete_button.pack(side="right")

        hint = ttk.Label(
            frame,
            text="The name and the metadata are stored with every sample of the "
            "person, so saving changes all of them.",
            foreground="#555555",
            wraplength=460,
            justify="left",
        )
        hint.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(8, 0))

    def _build_status_bar(self, parent: ttk.Frame) -> None:
        self.status_var = tk.StringVar(value="Not connected.")
        bar = ttk.Frame(parent)
        bar.grid(row=3, column=0, sticky="ew", pady=(6, 0))
        ttk.Label(bar, textvariable=self.status_var).pack(side="left")

    # -- connecting ---------------------------------------------------------

    def connect(self) -> None:
        """Opens the database and loads the people."""
        dsn = self.dsn_var.get().strip()
        table = self.table_var.get().strip() or "face_enrollments"

        self.store.close()
        self.store = FaceStore(dsn, table)
        self.root.configure(cursor="watch")
        try:
            self.store.connect()
        except FaceDbError as exc:
            self._show_connection_state()
            self._set_status(f"Could not connect: {exc}")
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return
        finally:
            self.root.configure(cursor="")

        self._show_connection_state()
        self._set_status(f"Connected to {table}.")
        self.refresh()

    def _show_connection_state(self) -> None:
        """Enables the widgets that need a connection, and says what is missing."""
        connected = self.store.connected
        state = "normal" if connected else "disabled"

        self.save_button.configure(state=state)
        self.format_button.configure(state=state)
        self.delete_button.configure(state=state)
        self.refresh_button.configure(state=state)
        self.name_entry.configure(state=state)
        self.metadata_text.configure(state=state)

        missing = self.store.missing_properties
        if missing and connected:
            self.warning_var.set(
                "The table cannot keep: "
                + ", ".join(sorted(missing))
                + ". It predates those columns and the database user is not "
                "allowed to alter it, so they cannot be edited here."
            )
            self.warning_label.grid()
        else:
            self.warning_label.grid_remove()

        if "user_name" not in missing:
            self.name_entry.configure(state=state)
        else:
            self.name_entry.configure(state="disabled")

    # -- loading ------------------------------------------------------------

    def refresh(self) -> None:
        """Reloads the people from the database."""
        if not self.store.connected:
            return
        self.root.configure(cursor="watch")
        try:
            people = self.store.list_people()
        except FaceDbError as exc:
            self._set_status(f"Could not read the people: {exc}")
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return
        finally:
            self.root.configure(cursor="")

        self._people = people
        self._refresh_people()
        self._set_status(f"{len(people)} person/people enrolled.")

    def _refresh_people(self) -> None:
        """Fills the list of people, honouring the filter."""
        selected = self._selected_person_id()
        for item in self.people_tree.get_children():
            self.people_tree.delete(item)

        needle = self.filter_var.get().strip().lower()
        for person in getattr(self, "_people", []):
            if needle:
                haystack = f"{person.person_id} {person.user_name or ''}".lower()
                if needle not in haystack:
                    continue
            name = person.user_name if person.user_name else ""
            self.people_tree.insert(
                "",
                "end",
                iid=str(person.person_id),
                values=(person.person_id, name, person.samples),
            )

        # Keep the selection across a refresh, so that a change that was
        # just saved does not throw the user back to the top of the list.
        if selected is not None and self.people_tree.exists(str(selected)):
            self.people_tree.selection_set(str(selected))
            self.people_tree.focus(str(selected))
            self._load_person(selected)
        else:
            self._clear_detail()

    def _on_person_selected(self, _event: tk.Event) -> None:
        person_id = self._selected_person_id()
        if person_id is not None:
            self._load_person(person_id)

    def _load_person(self, person_id: int) -> None:
        """Loads the samples and the properties of a person."""
        try:
            samples = self.store.list_samples(person_id)
        except FaceDbError as exc:
            self._set_status(f"Could not read the samples: {exc}")
            return

        for item in self.samples_tree.get_children():
            self.samples_tree.delete(item)
        for sample in samples:
            self.samples_tree.insert(
                "",
                "end",
                iid=str(sample.id),
                values=(
                    sample.id,
                    sample.enrolled_at.strftime("%Y-%m-%d %H:%M:%S")
                    if sample.enrolled_at
                    else "",
                    "yes" if sample.photo else "-",
                ),
            )

        if samples:
            self.samples_tree.selection_set(str(samples[0].id))
            self.samples_tree.focus(str(samples[0].id))
            self._show_sample(samples[0])
        else:
            self._clear_sample()

        self._set_status(f"Person {person_id}: {len(samples)} sample(s).")

    def _on_sample_selected(self, _event: tk.Event) -> None:
        selection = self.samples_tree.selection()
        if not selection:
            return
        sample_id = int(selection[0])
        person_id = self._selected_person_id()
        if person_id is None:
            return
        try:
            samples = self.store.list_samples(person_id)
        except FaceDbError:
            return
        for sample in samples:
            if sample.id == sample_id:
                self._show_sample(sample)
                return

    def _show_sample(self, sample: Any) -> None:
        """Shows the picture and the properties of one sample."""
        self._photo_sample_id = sample.id
        self._photo_data = sample.photo
        self._show_photo(sample.photo)

        if "user_name" not in self.store.missing_properties:
            self.name_var.set(sample.user_name or "")
        if "metadata" not in self.store.missing_properties:
            self.metadata_text.delete("1.0", "end")
            self.metadata_text.insert("1.0", format_metadata(sample.metadata))

    def _show_photo(self, data: bytes | None) -> None:
        """Puts the picture of a face into the preview."""
        self._photo_image = None
        self.photo_label.configure(image="", text="no picture")
        self.save_photo_button.configure(state="disabled")
        if not data:
            return

        png = image_to_png(data)
        if png is None:
            self.photo_label.configure(text="cannot show the picture")
            return
        try:
            image = tk.PhotoImage(data=png)
        except tk.TclError:
            self.photo_label.configure(text="cannot show the picture")
            return

        # Tk does not keep a reference to the image of a label, so it
        # would be collected and the label would go blank. The editor
        # keeps it.
        self._photo_image = image
        self.photo_label.configure(image=image, text="")
        self.save_photo_button.configure(state="normal")

    def _clear_sample(self) -> None:
        self._photo_sample_id = None
        self._photo_data = None
        self._show_photo(None)
        self.name_var.set("")
        self.metadata_text.delete("1.0", "end")

    def _clear_detail(self) -> None:
        for item in self.samples_tree.get_children():
            self.samples_tree.delete(item)
        self._clear_sample()

    def _selected_person_id(self) -> int | None:
        selection = self.people_tree.selection()
        if not selection:
            return None
        try:
            return int(selection[0])
        except ValueError:
            return None

    # -- writing ------------------------------------------------------------

    def save_changes(self) -> None:
        """Stores the name and the metadata of the selected person."""
        person_id = self._selected_person_id()
        if person_id is None:
            self._set_status("Select a person first.")
            return

        name = self.name_var.get().strip() or None
        text = self.metadata_text.get("1.0", "end-1c")
        try:
            metadata = parse_metadata(text)
        except FaceDbError as exc:
            self._set_status(str(exc))
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return

        missing = self.store.missing_properties
        try:
            if "user_name" not in missing:
                self.store.set_name(person_id, name)
            if "metadata" not in missing:
                self.store.set_metadata(person_id, metadata)
        except FaceDbError as exc:
            self._set_status(f"Could not save: {exc}")
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return

        self._set_status(f"Saved the properties of person {person_id}.")
        self.refresh()

    def format_metadata(self) -> None:
        """Rewrites the metadata in a readable shape."""
        try:
            metadata = parse_metadata(self.metadata_text.get("1.0", "end-1c"))
        except FaceDbError as exc:
            self._set_status(str(exc))
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return
        self.metadata_text.delete("1.0", "end")
        self.metadata_text.insert("1.0", format_metadata(metadata))

    def save_photo(self) -> None:
        """Saves the picture of the selected face to a file."""
        if not self._photo_data:
            self._set_status("There is no picture to save.")
            return

        person_id = self._selected_person_id()
        sample_id = self._photo_sample_id
        kind = sniff_image_format(self._photo_data)
        extension = {"jpeg": ".jpg", "png": ".png"}.get(kind, ".bin")

        path = filedialog.asksaveasfilename(
            parent=self.root,
            title="Save the picture of the face",
            initialfile=f"person-{person_id}-sample-{sample_id}{extension}",
            defaultextension=extension,
            filetypes=[
                ("JPEG image", "*.jpg *.jpeg"),
                ("PNG image", "*.png"),
                ("All files", "*"),
            ],
        )
        if not path:
            return

        try:
            with open(path, "wb") as handle:
                handle.write(self._photo_data)
        except OSError as exc:
            messagebox.showerror("Face database editor", f"Could not write {path}: {exc}", parent=self.root)
            return

        self._set_status(f"Saved the picture to {path} ({kind}).")

    def delete_person(self) -> None:
        """Removes every sample of the selected person."""
        person_id = self._selected_person_id()
        if person_id is None:
            self._set_status("Select a person first.")
            return

        name = self.name_var.get().strip()
        who = f"{name} ({person_id})" if name else str(person_id)
        if not messagebox.askyesno(
            "Face database editor",
            f"Remove every sample of {who}?\n\n"
            "The person cannot be recognised any more.",
            parent=self.root,
        ):
            return

        try:
            removed = self.store.remove_person(person_id)
        except FaceDbError as exc:
            self._set_status(f"Could not remove the person: {exc}")
            messagebox.showerror("Face database editor", str(exc), parent=self.root)
            return

        self._set_status(f"Removed {removed} sample(s) of person {person_id}.")
        self.refresh()

    # -- helpers ------------------------------------------------------------

    def _set_status(self, message: str) -> None:
        self.status_var.set(message)


def find_dsn(argument: str | None) -> str | None:
    """The connection string to use: the argument, else the environment."""
    if argument:
        return argument
    for name in DSN_ENV_VARS:
        value = os.environ.get(name)
        if value:
            return value
    return None


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="face_db_editor.py",
        description="Review and correct the enrolled faces of objective-control.",
    )
    parser.add_argument(
        "--dsn",
        metavar="CONNECTION",
        help=(
            "the PostgreSQL database to open, e.g. "
            "postgres://user@localhost/objective_control. Taken from $FACE_DB_URL "
            "or $DATABASE_URL when it is not given."
        ),
    )
    parser.add_argument(
        "--table",
        default="face_enrollments",
        help="the table that holds the enrolments (default: face_enrollments)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    dsn = find_dsn(args.dsn)
    if not dsn:
        print(
            "No database was named. Use --dsn=..., or set $FACE_DB_URL or "
            "$DATABASE_URL.",
            file=sys.stderr,
        )
        return 2

    try:
        import psycopg  # noqa: F401
    except ImportError:
        print(
            "The psycopg module is missing. Install it with:\n"
            "    pip install -r requirements.txt",
            file=sys.stderr,
        )
        return 1

    try:
        root = tk.Tk()
    except tk.TclError as exc:
        print(
            f"No display is available, so the editor cannot open a window ({exc}).\n"
            "It needs a running X server or a Wayland session.",
            file=sys.stderr,
        )
        return 1

    FaceDbEditor(root, dsn, args.table)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
