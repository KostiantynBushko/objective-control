/* The browser side of the face database editor.
 *
 * The page is built from the parts of the desktop editor: the list of
 * the people on the left, the samples of the selected person, the
 * picture of the selected face, the name, the metadata and the
 * embedding on the right. Everything the browser shows comes from the
 * server, and everything the user changes is sent back to it.
 */

'use strict';

const state = {
    connected: false,
    people: [],
    selectedPerson: null,
    samples: [],
    selectedSample: null,
    embedding: null,
    photoUrl: null,
    mergeSources: new Set(),
    mergeTarget: null,
};

const el = (id) => document.getElementById(id);

/* -- talking to the server --------------------------------------------- */

/* Every answer of the server carries the message of an error in the
 * field "error". It is shown in the bar at the bottom, so that the user
 * knows what went wrong without the browser saying nothing at all. */
async function api(path, options = {}) {
    const response = await fetch(path, {
        headers: { 'Content-Type': 'application/json' },
        ...options,
    });

    let payload = null;
    const type = response.headers.get('content-type') || '';
    if (type.includes('application/json')) {
        payload = await response.json();
    }

    if (!response.ok) {
        const message = (payload && payload.error) || `the server said ${response.status}`;
        const error = new Error(message);
        error.status = response.status;
        throw error;
    }
    return payload;
}

/* -- the bar at the bottom ---------------------------------------------- */

function setStatus(message, isError = false) {
    const bar = el('status');
    bar.textContent = message;
    bar.classList.toggle('error', isError);
}

function reportError(error) {
    setStatus(error.message || String(error), true);
}

/* -- the state of the connection ---------------------------------------- */

async function loadStatus() {
    let status;
    try {
        status = await api('/api/status');
    } catch (error) {
        reportError(error);
        return;
    }
    applyStatus(status);
}

function applyStatus(status) {
    state.connected = status.connected;

    el('dsn').textContent = status.dsn || '—';
    el('table').textContent = status.table || '—';

    const pill = el('connection');
    pill.textContent = status.connected ? 'connected' : 'not connected';
    pill.className = status.connected ? 'pill pill-on' : 'pill pill-off';

    el('connect').disabled = status.connected;
    el('disconnect').disabled = !status.connected;

    const missing = status.missing_properties || [];
    const warning = el('warning');
    if (status.connected && missing.length) {
        warning.textContent =
            `The table cannot keep: ${missing.join(', ')}. It predates those ` +
            'columns and the database user is not allowed to alter it, so they ' +
            'cannot be edited here.';
        warning.hidden = false;
    } else {
        warning.hidden = true;
    }

    /* Without a database nothing can be read or written, so the widgets
     * that need one are put out of the way. */
    for (const id of ['save', 'format', 'refresh', 'delete', 'merge', 'name', 'metadata']) {
        el(id).disabled = !status.connected;
    }
    el('copy-embedding').disabled = !status.connected || !state.embedding;
    el('save-photo').disabled = !status.connected || !state.photoUrl;
}

async function connect() {
    setStatus('Opening the database…');
    try {
        const status = await api('/api/connect', { method: 'POST' });
        applyStatus(status);
        setStatus(`Connected to ${status.table}.`);
        await loadPeople();
    } catch (error) {
        reportError(error);
        await loadStatus();
    }
}

async function disconnect() {
    try {
        const status = await api('/api/disconnect', { method: 'POST' });
        applyStatus(status);
        setStatus('Disconnected.');
        clearPeople();
    } catch (error) {
        reportError(error);
    }
}

/* -- the people --------------------------------------------------------- */

async function loadPeople() {
    let payload;
    try {
        payload = await api('/api/people');
    } catch (error) {
        reportError(error);
        return;
    }

    state.people = payload.people || [];
    renderPeople();
    setStatus(`${state.people.length} person/people enrolled.`);

    /* The first person is picked when nobody is picked yet, so that the
     * page shows something instead of an empty form. The desktop editor
     * leaves the form empty, but there the list can be walked with the
     * keyboard, and here it cannot. */
    if (state.selectedPerson === null && state.people.length) {
        await selectPerson(state.people[0].person_id);
    }
}

function clearPeople() {
    state.people = [];
    state.selectedPerson = null;
    state.samples = [];
    state.selectedSample = null;
    state.embedding = null;
    state.photoUrl = null;
    renderPeople();
    renderSamples();
    showSample(null);
}

function filteredPeople() {
    const needle = el('filter').value.trim().toLowerCase();
    if (!needle) {
        return state.people;
    }
    return state.people.filter((person) => {
        const haystack = `${person.person_id} ${person.user_name || ''}`.toLowerCase();
        return haystack.includes(needle);
    });
}

function renderPeople() {
    const body = el('people-body');
    body.textContent = '';

    const shown = filteredPeople();
    for (const person of shown) {
        const row = document.createElement('tr');
        row.dataset.personId = person.person_id;
        if (state.selectedPerson === person.person_id) {
            row.classList.add('selected');
        }

        const id = document.createElement('td');
        id.className = 'col-id';
        id.textContent = person.person_id;

        const name = document.createElement('td');
        name.textContent = person.user_name || '';

        const samples = document.createElement('td');
        samples.className = 'col-count';
        samples.textContent = person.samples;

        row.append(id, name, samples);
        row.addEventListener('click', () => selectPerson(person.person_id));
        body.append(row);
    }

    const empty = el('people-empty');
    if (!state.connected) {
        empty.hidden = true;
    } else if (shown.length) {
        empty.hidden = true;
    } else if (state.people.length) {
        empty.textContent = 'No person matches the filter.';
        empty.hidden = false;
    } else {
        empty.textContent =
            'Nobody is enrolled in this table yet.\n\n' +
            'The application writes the rows when it enrols a face:\n' +
            '  objective-control-app --faces --enroll=1 \\\n' +
            '      --face-db=<this connection string>';
        empty.hidden = false;
    }
}

async function selectPerson(personId) {
    state.selectedPerson = personId;
    renderPeople();

    let payload;
    try {
        payload = await api(`/api/people/${personId}`);
    } catch (error) {
        reportError(error);
        return;
    }

    state.samples = payload.samples || [];
    renderSamples();
    setStatus(`Person ${personId}: ${state.samples.length} sample(s).`);

    if (state.samples.length) {
        selectSample(state.samples[0].id);
    } else {
        showSample(null);
    }
}

/* -- the samples of the selected person --------------------------------- */

function renderSamples() {
    const body = el('samples-body');
    body.textContent = '';

    for (const sample of state.samples) {
        const row = document.createElement('tr');
        row.dataset.sampleId = sample.id;
        if (state.selectedSample === sample.id) {
            row.classList.add('selected');
        }

        const id = document.createElement('td');
        id.className = 'col-id';
        id.textContent = sample.id;

        const enrolled = document.createElement('td');
        enrolled.textContent = sample.enrolled_at ? sample.enrolled_at.replace('T', ' ').slice(0, 19) : '';

        const photo = document.createElement('td');
        photo.className = 'col-count';
        photo.textContent = sample.has_photo ? 'yes' : '-';

        row.append(id, enrolled, photo);
        row.addEventListener('click', () => selectSample(sample.id));
        body.append(row);
    }
}

function selectSample(sampleId) {
    const sample = state.samples.find((item) => item.id === sampleId);
    if (!sample) {
        return;
    }
    state.selectedSample = sampleId;
    renderSamples();
    showSample(sample);
}

function showSample(sample) {
    const image = el('face');
    const empty = el('face-empty');

    state.embedding = null;
    state.photoUrl = null;

    if (!sample) {
        image.hidden = true;
        image.removeAttribute('src');
        empty.textContent = 'no picture';
        empty.hidden = false;
        el('name').value = '';
        el('metadata').value = '';
        renderEmbedding(null);
        el('save-photo').disabled = true;
        el('copy-embedding').disabled = true;
        return;
    }

    el('name').value = sample.user_name || '';
    el('metadata').value = formatJson(sample.metadata);

    if (sample.has_photo) {
        const url = `/api/people/${sample.person_id}/samples/${sample.id}/photo`;
        state.photoUrl = url;
        image.src = url;
        image.hidden = false;
        empty.hidden = true;
        el('save-photo').disabled = false;
    } else {
        image.hidden = true;
        image.removeAttribute('src');
        empty.textContent = 'no picture';
        empty.hidden = false;
        el('save-photo').disabled = true;
    }

    renderEmbedding(sample.embedding, sample.embedding_length);
}

/* The metadata comes from the server as a value. It is shown as the
 * text the user can edit, and it is formatted the way the server
 * formats it. */
function formatJson(value) {
    if (value === null || value === undefined) {
        return '{}';
    }
    if (typeof value === 'string') {
        return JSON.stringify(value, null, 2);
    }
    return JSON.stringify(value, null, 2);
}

/* -- the embedding ------------------------------------------------------ */

function renderEmbedding(values, length) {
    const box = el('embedding');
    const summary = el('embedding-summary');
    const lengthLabel = el('embedding-length');

    if (!values || !values.length) {
        box.textContent = '';
        summary.textContent = 'no embedding';
        lengthLabel.textContent = '';
        el('copy-embedding').disabled = true;
        state.embedding = null;
        return;
    }

    state.embedding = values.slice();
    box.textContent = values
        .map((value, index) => `${String(index).padStart(3)}  ${Number(value).toFixed(6)}`)
        .join('\n');

    summary.textContent = `${values.length} values`;
    lengthLabel.textContent = length === null || length === undefined
        ? ''
        : `length ${Number(length).toFixed(6)}`;
    el('copy-embedding').disabled = !state.connected;
}

async function copyEmbedding() {
    if (!state.embedding) {
        setStatus('There is no embedding to copy.');
        return;
    }
    const text = JSON.stringify(state.embedding);
    try {
        await navigator.clipboard.writeText(text);
        setStatus(`Copied the ${state.embedding.length} values of the embedding.`);
    } catch (error) {
        /* The browser does not allow the clipboard without a user
         * gesture, and some browsers do not allow it at all. The text is
         * shown then, so that it can be taken by hand. */
        const box = el('embedding');
        const selection = window.getSelection();
        const range = document.createRange();
        range.selectNodeContents(box);
        selection.removeAllRanges();
        selection.addRange(range);
        setStatus('The clipboard is not allowed here; the values are selected.', true);
    }
}

/* -- storing the changes ------------------------------------------------ */

async function saveChanges() {
    if (state.selectedPerson === null) {
        setStatus('Select a person first.');
        return;
    }

    const body = {
        name: el('name').value.trim() || null,
        metadata: el('metadata').value,
    };

    try {
        const result = await api(`/api/people/${state.selectedPerson}`, {
            method: 'PUT',
            body: JSON.stringify(body),
        });
        const updated = Object.keys(result.updated || {}).length;
        setStatus(`Saved the properties of person ${state.selectedPerson}.`);
        if (!updated && (result.skipped || []).length) {
            setStatus(
                `Nothing was stored: the table cannot keep ${result.skipped.join(', ')}.`,
                true,
            );
        }
        await loadPeople();
        await selectPerson(state.selectedPerson);
    } catch (error) {
        reportError(error);
    }
}

async function formatMetadata() {
    const text = el('metadata').value;
    try {
        const result = await api('/api/metadata/format', {
            method: 'POST',
            body: JSON.stringify({ text }),
        });
        el('metadata').value = result.text;
        setStatus('The metadata was rewritten in a readable shape.');
    } catch (error) {
        reportError(error);
    }
}

async function deletePerson() {
    if (state.selectedPerson === null) {
        setStatus('Select a person first.');
        return;
    }

    const name = el('name').value.trim();
    const who = name ? `${name} (${state.selectedPerson})` : String(state.selectedPerson);
    if (!window.confirm(`Remove every sample of ${who}?\n\nThe person cannot be recognised any more.`)) {
        return;
    }

    try {
        const result = await api(`/api/people/${state.selectedPerson}`, { method: 'DELETE' });
        setStatus(`Removed ${result.removed} sample(s) of person ${state.selectedPerson}.`);
        state.selectedPerson = null;
        await loadPeople();
        clearSamples();
    } catch (error) {
        reportError(error);
    }
}

function clearSamples() {
    state.samples = [];
    state.selectedSample = null;
    renderSamples();
    showSample(null);
}

/* -- the picture of a face ---------------------------------------------- */

async function savePhoto() {
    if (!state.photoUrl || state.selectedSample === null) {
        setStatus('There is no picture to save.');
        return;
    }

    try {
        const response = await fetch(state.photoUrl);
        if (!response.ok) {
            throw new Error(`the server said ${response.status}`);
        }
        const blob = await response.blob();
        const kind = blob.type === 'image/png' ? 'png' : 'jpeg';
        const name = `person-${state.selectedPerson}-sample-${state.selectedSample}.${kind === 'png' ? 'png' : 'jpg'}`;

        const url = URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = url;
        link.download = name;
        document.body.append(link);
        link.click();
        link.remove();
        URL.revokeObjectURL(url);

        setStatus(`Saved the picture to ${name} (${kind}).`);
    } catch (error) {
        reportError(error);
    }
}

/* -- the merge ---------------------------------------------------------- */

function openMerge() {
    if (state.selectedPerson === null) {
        setStatus('Select a person first.');
        return;
    }
    if (!state.connected) {
        setStatus('The editor is not connected to a database.');
        return;
    }

    const target = state.people.find((person) => person.person_id === state.selectedPerson);
    const others = state.people.filter((person) => person.person_id !== state.selectedPerson);

    if (!others.length) {
        setStatus('There is nobody else in this table to merge with.');
        return;
    }

    state.mergeTarget = target;
    state.mergeSources = new Set();

    const name = target && target.user_name ? target.user_name : '(no name)';
    el('merge-headline').textContent =
        `The samples of the people below are moved into person ${target.person_id} (${name}).\n` +
        'The person keeps its id, and the rows keep the time they were enrolled.';

    const body = el('merge-body');
    body.textContent = '';
    for (const person of others) {
        const row = document.createElement('tr');

        const check = document.createElement('td');
        check.className = 'col-check';
        const box = document.createElement('input');
        box.type = 'checkbox';
        box.value = person.person_id;
        box.addEventListener('change', () => {
            if (box.checked) {
                state.mergeSources.add(person.person_id);
            } else {
                state.mergeSources.delete(person.person_id);
            }
            updateMergeSummary();
        });
        check.append(box);

        const id = document.createElement('td');
        id.className = 'col-id';
        id.textContent = person.person_id;

        const nameCell = document.createElement('td');
        nameCell.textContent = person.user_name || '';

        const samples = document.createElement('td');
        samples.className = 'col-count';
        samples.textContent = person.samples;

        row.append(check, id, nameCell, samples);
        body.append(row);
    }

    el('merge-overlay').hidden = false;
    updateMergeSummary();
}

function updateMergeSummary() {
    const summary = el('merge-summary');
    const confirm = el('merge-confirm');

    if (!state.mergeSources.size) {
        summary.textContent = 'Pick the people that are the same person as the one above.';
        confirm.disabled = true;
        return;
    }

    const sources = [...state.mergeSources].join(',');
    api(`/api/people/${state.mergeTarget.person_id}/merge-preview?sources=${sources}`)
        .then((preview) => {
            summary.textContent = preview.summary;
            confirm.disabled = false;
        })
        .catch((error) => {
            summary.textContent = error.message;
            confirm.disabled = true;
        });
}

function closeMerge() {
    el('merge-overlay').hidden = true;
    state.mergeSources = new Set();
    state.mergeTarget = null;
}

async function confirmMerge() {
    if (!state.mergeTarget || !state.mergeSources.size) {
        return;
    }
    const targetId = state.mergeTarget.person_id;
    const sources = [...state.mergeSources];

    try {
        const result = await api(`/api/people/${targetId}/merge`, {
            method: 'POST',
            body: JSON.stringify({ sources }),
        });
        closeMerge();
        setStatus(
            `Moved ${result.moved} sample(s) to person ${result.target_id} ` +
            `(${result.kept} sample(s) were there already).`,
        );
        await loadPeople();
        await selectPerson(targetId);
    } catch (error) {
        reportError(error);
    }
}

/* -- the wiring --------------------------------------------------------- */

function onReady() {
    el('filter').addEventListener('input', renderPeople);
    el('connect').addEventListener('click', connect);
    el('disconnect').addEventListener('click', disconnect);
    el('save').addEventListener('click', saveChanges);
    el('format').addEventListener('click', formatMetadata);
    el('delete').addEventListener('click', deletePerson);
    el('refresh').addEventListener('click', async () => {
        await loadPeople();
        if (state.selectedPerson !== null) {
            await selectPerson(state.selectedPerson);
        }
    });
    el('merge').addEventListener('click', openMerge);
    el('merge-cancel').addEventListener('click', closeMerge);
    el('merge-confirm').addEventListener('click', confirmMerge);
    el('copy-embedding').addEventListener('click', copyEmbedding);
    el('save-photo').addEventListener('click', savePhoto);

    /* The escape key closes the window of the merge. */
    document.addEventListener('keydown', (event) => {
        if (event.key === 'Escape' && !el('merge-overlay').hidden) {
            closeMerge();
        }
    });

    loadStatus().then(async () => {
        if (state.connected) {
            await loadPeople();
        }
    });
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', onReady);
} else {
    onReady();
}
