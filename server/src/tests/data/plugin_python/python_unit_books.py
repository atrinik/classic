"""Exercise finalized books through the real Python Object mutation API."""

import Atrinik


signature = {
    "book_signer": "Plugin book writer",
    "book_signed_date": "28 Month of the Winter, Year 1",
    "book_signed_utc": "1791417600",
    "book_finalized": "1",
}


def snapshot(book):
    return (
        book.name,
        book.msg,
        book.custom_name,
        book.title,
        book.type,
        {key: book.ReadKey(key) for key in signature},
    )


def reject(book, action):
    before = snapshot(book)
    try:
        action()
    except RuntimeError as error:
        if "Finalized book" not in str(error):
            raise
    else:
        raise RuntimeError("Finalized book mutation was accepted")
    if snapshot(book) != before:
        raise RuntimeError("Rejected book mutation changed its signed record")


book = Atrinik.CreateObject("book")
book.name = "Plugin signed title"
book.msg = "Plugin signed contents"
book.custom_name = "Quest alias"
book.title = "of a quest"
# Unsigned quest scripts retain both the direct setters and bulk load route.
book.Load("name Unsigned scripted title\nmsg\nUnsigned scripted contents\nendmsg\n")
if (book.name, book.msg) != ("Unsigned scripted title", "Unsigned scripted contents"):
    raise RuntimeError("Unsigned script editing failed")
book.name = "Plugin signed title"
book.msg = "Plugin signed contents"
for key, value in signature.items():
    if not book.WriteKey(key, value):
        raise RuntimeError("Could not prepare signed book fixture")


def check_finalized(book):
    for field, value in (
        ("name", "Tampered title"),
        ("msg", "Tampered contents"),
        ("msg", None),
        ("custom_name", "Tampered alias"),
        ("custom_name", None),
        ("title", "of tampering"),
        ("type", Atrinik.Type.MISC_OBJECT),
    ):
        reject(book, lambda: setattr(book, field, value))
    # Both overwriting and deleting any signature component must fail atomically.
    for key in signature:
        reject(book, lambda: book.WriteKey(key, "tampered"))
        reject(book, lambda: book.WriteKey(key))
        reject(book, lambda: book.WriteKey(key, "", False))
    for key in (
        "name", "name_pl", "msg", "custom_name", "title", "type", "arch",
        "artifact", "object", "more", "end", "endmsg", "name injected",
        "quest\nbook_finalized",
    ):
        reject(book, lambda: book.WriteKey(key, "tampered"))
    for value in ("quest\nname Tampered title", "quest\rbook_finalized 0"):
        reject(book, lambda: book.WriteKey("quest_state", value))
    reject(book, lambda: book.Load("name Tampered title\n"))
    reject(book, lambda: book.Load("book_finalized 0\nmsg\nTampered\nendmsg\n"))
    reject(book, lambda: book.Artificate("nonexistent-book-artifact"))
    # Authored hooks can still advance quest state and modify unrelated fields.
    book.WriteKey("quest_state", "read")
    book.value += 1
    if book.ReadKey("quest_state") != "read":
        raise RuntimeError("Finalization blocked normal quest metadata")


check_finalized(book)
original = snapshot(book)
for inventory in (False, True):
    clone = book.Clone(inventory)
    if snapshot(clone) != original:
        raise RuntimeError("Clone did not retain signed book record")
    check_finalized(clone)
    clone.Destroy()

loaded = Atrinik.LoadObject(book.Save())
if not loaded or snapshot(loaded) != original:
    raise RuntimeError("Reload did not retain signed book record")
check_finalized(loaded)
loaded.Destroy()

# Finalization follows the marker even on a detached object with an unexpected
# value. It cannot be bypassed by changing the book's mutable type first.
unexpected = Atrinik.CreateObject("book")
unexpected.WriteKey("book_finalized", "unexpected")
check_finalized(unexpected)
unexpected.Destroy()

# Inventory insertion preserves the same guards without freezing quest hooks.
book.InsertInto(Atrinik.WhoIsActivator())
check_finalized(book)
book.Destroy()
