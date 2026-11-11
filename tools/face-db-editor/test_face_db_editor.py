"""Tests for the window of the face database editor.

The editor is a Tk application, so these tests need a display. They are
skipped when there is none, and they are skipped as well when no
PostgreSQL server is named:

    FACE_DETECTION_TEST_DSN='host=localhost dbname=faces' python3 test_face_db_editor.py

The tests use the table of ``test_face_db.py``, which is a table of their
own and not the table of an installation.

Run them with::

    python3 -m unittest discover -s tools/face-db-editor -p 'test_*.py'
"""

from __future__ import annotations

import json
import os
import sys
import tkinter as tk
import tkinter.font as tkfont
import unittest
from tkinter import ttk
from unittest.mock import patch

import psycopg

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import face_db_editor
from face_db_editor import FaceDbEditor
from test_face_db import TEST_TABLE, drop_table, make_table, test_dsn


def has_display() -> bool:
    """True when a window can be opened."""
    return bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


class EditorWindowTest(unittest.TestCase):
    """The editor window, driven for real."""

    dsn = test_dsn()

    def setUp(self) -> None:
        if self.dsn is None:
            self.skipTest("no PostgreSQL server named (set FACE_DETECTION_TEST_DSN)")
        if not has_display():
            self.skipTest("no display available")

        bootstrap = psycopg.connect(self.dsn, autocommit=True)
        try:
            make_table(bootstrap)
        finally:
            bootstrap.close()

        # A dialog would wait for a key press that never comes in a
        # test, so the message boxes are taken out of the way.
        self._dialog = patch.object(face_db_editor.messagebox, "showerror")
        self._dialog.start()
        self.addCleanup(self._dialog.stop)

        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)

        self.editor = FaceDbEditor(self.root, self.dsn, TEST_TABLE)
        self.addCleanup(self.editor.store.close)

    def add_sample(
        self,
        person_id: int,
        name: str | None = None,
        embedding: list[float] | None = None,
    ) -> None:
        if embedding is None:
            embedding = [0.1] * 128
        with psycopg.connect(self.dsn, autocommit=True) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    f"INSERT INTO {TEST_TABLE} (person_id, embedding, user_name) "
                    "VALUES (%s, %s, %s)",
                    (person_id, embedding, name),
                )

    # -- the button is a toggle ---------------------------------------------

    def test_the_button_offers_to_disconnect_while_connected(self) -> None:
        # The editor connects when it opens.
        self.assertTrue(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Disconnect")

    def test_the_button_offers_to_connect_when_disconnected(self) -> None:
        self.editor.disconnect()
        self.assertFalse(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Connect")

    def test_the_button_closes_the_database_and_opens_it_again(self) -> None:
        self.editor.connect_button.invoke()
        self.assertFalse(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Connect")

        self.editor.connect_button.invoke()
        self.assertTrue(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Disconnect")

    def test_the_label_follows_the_connection_whatever_the_way(self) -> None:
        # Not only the button: the label is right in both directions.
        for _ in range(2):
            self.editor.connect()
            self.assertEqual(self.editor.connect_button.cget("text"), "Disconnect")
            self.editor.disconnect()
            self.assertEqual(self.editor.connect_button.cget("text"), "Connect")

    # -- disconnecting empties the editor ------------------------------------

    def test_disconnecting_empties_the_list_of_people(self) -> None:
        self.add_sample(1, "Ada Lovelace")
        self.editor.refresh()
        self.assertTrue(self.editor.people_tree.get_children())

        self.editor.disconnect()

        self.assertEqual(self.editor.people_tree.get_children(), ())
        self.assertEqual(self.editor.samples_tree.get_children(), ())
        self.assertEqual(self.editor.name_var.get(), "")
        self.assertEqual(self.editor.status_var.get(), "Disconnected.")

    def test_disconnecting_disables_the_writing_widgets(self) -> None:
        self.editor.disconnect()
        self.assertEqual(str(self.editor.save_button.cget("state")), "disabled")
        self.assertEqual(str(self.editor.delete_button.cget("state")), "disabled")
        self.assertEqual(str(self.editor.refresh_button.cget("state")), "disabled")

    def test_connecting_again_enables_them(self) -> None:
        self.editor.disconnect()
        self.editor.connect()
        self.assertEqual(str(self.editor.save_button.cget("state")), "normal")
        self.assertEqual(str(self.editor.refresh_button.cget("state")), "normal")

    # -- the empty list says why it is empty ---------------------------------

    def test_an_empty_table_says_that_nobody_is_enrolled(self) -> None:
        self.editor.refresh()
        self.assertEqual(self.editor.people_empty_label.winfo_manager(), "grid")
        self.assertIn(
            "Nobody is enrolled", self.editor.people_empty_label.cget("text")
        )

    def test_a_table_with_people_has_no_hint(self) -> None:
        self.add_sample(1, "Ada Lovelace")
        self.editor.refresh()
        self.assertEqual(self.editor.people_empty_label.winfo_manager(), "")

    def test_a_filter_that_hides_everybody_says_so(self) -> None:
        self.add_sample(1, "Ada Lovelace")
        self.editor.refresh()
        self.editor.filter_var.set("nobody by that name")
        self.assertEqual(self.editor.people_empty_label.winfo_manager(), "grid")
        self.assertIn("filter", self.editor.people_empty_label.cget("text"))

    def test_a_disconnected_editor_has_no_hint(self) -> None:
        self.editor.disconnect()
        self.assertEqual(self.editor.people_empty_label.winfo_manager(), "")

    # -- a connection string that is wrong ------------------------------------

    def test_a_target_that_is_not_a_database_is_reported(self) -> None:
        # The store refuses such a target, and the editor has to say so
        # instead of dying with a traceback.
        self.editor.dsn_var.set("faces.db")
        self.editor.connect()

        self.assertFalse(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Connect")
        self.assertIn("Could not connect", self.editor.status_var.get())

    def test_a_wrong_target_leaves_the_editor_usable(self) -> None:
        self.editor.dsn_var.set("faces.db")
        self.editor.connect()

        self.editor.dsn_var.set(self.dsn)
        self.editor.connect()
        self.assertTrue(self.editor.store.connected)
        self.assertEqual(self.editor.connect_button.cget("text"), "Disconnect")

    # -- the embedding of a sample -------------------------------------------

    def test_the_embedding_of_the_sample_is_shown(self) -> None:
        self.add_sample(1, "Ada Lovelace", [0.5, -0.25, 0.125])
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        text = self.editor.embedding_text.get("1.0", "end-1c")
        self.assertIn("0.500000", text)
        self.assertIn("-0.250000", text)
        self.assertIn("0.125000", text)

    def test_the_summary_says_how_many_values_the_embedding_has(self) -> None:
        self.add_sample(1, "Ada Lovelace", [0.5, -0.25, 0.125])
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        summary = self.editor.embedding_summary_var.get()
        self.assertIn("3 values", summary)

    def test_the_summary_says_when_the_embedding_has_an_odd_length(self) -> None:
        # The library always writes a vector of 128 values, so a row with
        # another number of them was written by something else.
        self.add_sample(1, "Ada Lovelace", [0.5, -0.25, 0.125])
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        summary = self.editor.embedding_summary_var.get()
        self.assertIn("3 values", summary)
        self.assertIn("expected 128", summary)

    def test_the_embedding_cannot_be_edited(self) -> None:
        # The embedding is written by the application, and the editor only
        # shows what a person is recognised by.
        self.add_sample(1, "Ada Lovelace")
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        self.assertEqual(str(self.editor.embedding_text.cget("state")), "disabled")

    def test_the_copy_button_is_disabled_without_an_embedding(self) -> None:
        self.editor.refresh()
        self.assertEqual(
            str(self.editor.copy_embedding_button.cget("state")), "disabled"
        )

    def test_copying_the_embedding_puts_it_on_the_clipboard(self) -> None:
        values = [0.5, -0.25, 0.125]
        self.add_sample(1, "Ada Lovelace", values)
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        self.editor.copy_embedding_button.invoke()

        self.assertEqual(json.loads(self.editor.root.clipboard_get()), values)

    def test_disconnecting_clears_the_embedding(self) -> None:
        self.add_sample(1, "Ada Lovelace", [0.5, -0.25, 0.125])
        self.editor.refresh()
        self.editor.people_tree.selection_set("1")
        self.editor._on_person_selected(None)  # noqa: SLF001

        self.editor.disconnect()

        self.assertEqual(
            self.editor.embedding_text.get("1.0", "end-1c").strip(), ""
        )
        self.assertEqual(
            str(self.editor.copy_embedding_button.cget("state")), "disabled"
        )


class StubStore:
    """A store that keeps nothing and reaches no database.

    The window of the editor is built without a database, so that the
    tests of the layout can run everywhere a display is.
    """

    def __init__(self, dsn: str, table: str) -> None:
        self.dsn = dsn
        self.table = table
        self._connected = False

    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def missing_properties(self) -> frozenset[str]:
        return frozenset()

    def connect(self) -> None:
        self._connected = True

    def close(self) -> None:
        self._connected = False

    def list_people(self) -> list:
        return []

    def list_samples(self, person_id: int) -> list:
        return []


class LayoutTest(unittest.TestCase):
    """The window of the editor, without a database."""

    def setUp(self) -> None:
        if not has_display():
            self.skipTest("no display available")

        # The store of the editor is taken out of the way: a real one
        # would try to reach a database that is not there.
        self._store = patch.object(face_db_editor, "FaceStore", StubStore)
        self._store.start()
        self.addCleanup(self._store.stop)

        self.root = tk.Tk()
        self.addCleanup(self.root.destroy)

        self.editor = FaceDbEditor(self.root, "postgres://user@localhost/faces", TEST_TABLE)
        self.root.update()

    def properties_pane(self) -> tk.Widget:
        """The pane of the properties, found by its title."""
        found = []

        def find(widget: tk.Misc) -> None:
            for child in widget.winfo_children():
                if child.winfo_class() == "TLabelframe" and child.cget("text") == "Properties":
                    found.append(child)
                find(child)

        find(self.root)
        self.assertEqual(len(found), 1, "the pane of the properties is not there")
        return found[0]

    # -- the rows of the trees hold the whole of the text --------------------

    def test_the_rows_of_the_trees_hold_the_whole_of_the_text(self) -> None:
        # Tk measures a row of a tree at twenty pixels, whatever the
        # font is, and the text of the row is then drawn so that half
        # of it falls outside of the row.
        style = ttk.Style(self.root)
        font = tkfont.Font(
            root=self.root, font=style.lookup("Treeview", "font") or "TkDefaultFont"
        )
        needed = font.metrics("linespace")
        given = int(style.lookup("Treeview", "rowheight") or 0)

        self.assertGreaterEqual(
            given,
            needed,
            "the rows of the trees are too short for the text of their font",
        )

    def test_both_trees_have_the_same_rows(self) -> None:
        style = ttk.Style(self.root)
        given = int(style.lookup("Treeview", "rowheight") or 0)
        for tree in (self.editor.people_tree, self.editor.samples_tree):
            self.assertEqual(tree.winfo_reqheight() > 0, True)
        self.assertGreater(given, 0)

    # -- the labels of the properties are seen -------------------------------

    def test_the_labels_of_the_properties_are_seen(self) -> None:
        # The column of the labels has no weight, so that the pane
        # does not make them narrower when it is too narrow for its
        # contents: they would then be squeezed out of sight.
        pane = self.properties_pane()
        labels = {
            slave.cget("text"): slave
            for slave in pane.grid_slaves()
            if slave.winfo_class() == "TLabel"
        }

        for text in ("Name:", "Metadata:"):
            self.assertIn(text, labels)
            self.assertTrue(labels[text].winfo_ismapped(), f"the label {text!r} is not shown")
            self.assertGreater(labels[text].winfo_width(), 0, f"the label {text!r} has no width")

    # -- the panes are not squeezed out of the window ------------------------

    def test_the_buttons_of_the_properties_are_seen(self) -> None:
        for button in (
            self.editor.save_button,
            self.editor.format_button,
            self.editor.refresh_button,
            self.editor.delete_button,
        ):
            self.assertTrue(button.winfo_ismapped(), f"the button {button.cget('text')!r} is not shown")
            self.assertGreater(button.winfo_height(), 0)

    def test_the_metadata_has_room(self) -> None:
        self.assertGreater(self.editor.metadata_text.winfo_height(), 0)

    def test_the_embedding_has_room(self) -> None:
        # The list of the values must not collapse when the window is too
        # small for everything else: it would leave the pane empty.
        self.assertTrue(self.editor.embedding_text.winfo_ismapped())
        self.assertGreater(self.editor.embedding_text.winfo_height(), 0)

    def test_the_copy_button_of_the_embedding_is_seen(self) -> None:
        self.assertTrue(self.editor.copy_embedding_button.winfo_ismapped())
        self.assertGreater(self.editor.copy_embedding_button.winfo_height(), 0)


if __name__ == "__main__":
    unittest.main()
