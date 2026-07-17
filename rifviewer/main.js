/**
 * @param {string} error Error to show
 */
function showError(error) {
    const treeView = document.getElementById('tree-view');
    treeView.innerHTML = `<span style="color: red">${error}</span>`;
}

/**
 * @param {File} file
 * @return {Promise<ArrayBuffer>}
 */
function readFile(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader()
        reader.addEventListener("load", ev => {
            resolve(reader.result)
        })

        reader.addEventListener("error", ev => {
            reject(reader.error)
        })

        reader.readAsArrayBuffer(file)
    });
}

const td = new TextDecoder();

/**
 * 
 * @param {ArrayBuffer} buffer
 * @return {HTMLElement} 
 */
function getChunk(id, buffer) {
    let hasChildren = false;
    const data = {};
    switch (id) {
        case "CUTSCUSR":
        case "CUTSHEAD":
        case "CUTTRACK":
        case "DUMMYOBJ":
        case "FRAGTYPE":
        case "LIGHTSET":
        case "MODULEDT":
        case "OBANSEQC":
        case "OBANSEQS":
        case "OBJCHIER":
        case "OBJPRJDT":
        case "RBOBJECT":
        case "REBENVDT":
        case "REBINFF2":
        case "REBSHAPE":
        case "SPECLOBJ":
        case "SHPMORPH":
        case "SHPFRAGS":
        case "SPRIHEAD":
        case "SPRITEPC":
        case "SPRITEPS":
        case "SPRITESA":
            hasChildren = true;
            break;
        default: break;
    }

    const children = [];
    if (hasChildren) {
        while (buffer.byteLength > 0) {
            const dv = new DataView(buffer);
            const child_id = td.decode(buffer.slice(0, 8));
            const child_size = dv.getUint32(8, true);
            const child = getChunk(child_id, buffer.slice(12, child_size));
            children.push(child);
            buffer = buffer.slice(child_size);
        }

        return { id: id, data: {}, children: children };
    } else {
        const buf = new Uint8Array(buffer);

        return { id: id, data: buf, children: children };
    }
}

function createTree(chunk) {
    if (chunk.children.length) {
        const details = document.createElement("details");
        const summary = document.createElement("summary");
        const link = document.createElement("a");
        const content = document.createElement("div");
        link.addEventListener("click", () => {
            console.log(chunk);
        });
        link.href = "#";
        link.innerText = chunk.id;

        summary.appendChild(link);
        details.appendChild(summary);
        details.open = true;

        chunk.children.forEach(child => {
            content.appendChild(createTree(child))
        });
        content.classList.add("tree-content");
        details.appendChild(content);

        return details;
    } else {
        const link = document.createElement("a");
        const content = document.createElement("div");
        link.addEventListener("click", () => {
            console.log(chunk);
        });
        link.href = "#";
        link.innerText = chunk.id;

        content.appendChild(link);

        return content;
    }
}

/**
 * @param {FileList} files
 */
async function handleLoad(files) {
    const treeView = document.getElementById('tree-view');
    if (files.length == 0) {
        showError("Select a file to start");
    } else if (files.length > 1) {
        showError("Only one file can be shown at a time");
    };
    treeView.innerHTML = "";
    const file = files[0];
    const data = await readFile(file);
    const dv = new DataView(data);

    const id = td.decode(data.slice(0, 8));
    if (id == "REBCRIF1") {
        showError("Only uncompressed files are supported");
        return;
    }
    const size = dv.getUint32(8, true);

    treeView.appendChild(createTree(getChunk(id, data.slice(12, size))));
}