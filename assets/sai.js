function createPconDiv(pcon) {
    const pconDiv = document.createElement("div");
    pconDiv.className = "pcon";
    pconDiv.id = "pcon-" + pcon.name;
    pconDiv.style.marginLeft = "10px";
    pconDiv.style.borderLeft = "1px solid #ccc";
    pconDiv.style.paddingLeft = "5px";

    const header = document.createElement("div");
    header.className = "pcon-header";

    let stateClass = pcon.on ? "pcon-on" : "pcon-off";
    let type = pcon.type ? `(${pcon.type})` : "";

    header.innerHTML = `<span class="${stateClass}">&#x23FB;</span> <b>${hsanitize(pcon.name)}</b> <span class="pcon-type">${hsanitize(type)}</span>`;
    pconDiv.appendChild(header);

    const childrenDiv = document.createElement("div");
    childrenDiv.className = "pcon-children";
    pconDiv.appendChild(childrenDiv);

    return pconDiv;
}

/* Global caches for reconcilation */
var pcon_topology = {};
var last_builder_list = [];

function renderPconHierarchy(container) {
    if (!container) return;

    /* Clear and redraw for now to ensure structure is correct */
    container.innerHTML = "";

    const pcons = Object.values(pcon_topology);
    /* Build map for dependency resolution */
    const pconMap = {};
    pcons.forEach(p => {
        p.children = []; /* Reset children */
        pconMap[p.name] = p;
    });

    /* Link PCONs */
    const roots = [];
    pcons.forEach(p => {
        if (p.depends_on && pconMap[p.depends_on]) {
            pconMap[p.depends_on].children.push(p);
        } else {
            roots.push(p);
        }
    });

    /* Sort roots and children by name */
    const sortByName = (a, b) => a.name.localeCompare(b.name);
    roots.sort(sortByName);
    pcons.forEach(p => p.children.sort(sortByName));

    /* Helper to recursively render PCONs and their builders */
    function renderPcon(pcon, parentDiv) {
        const div = createPconDiv(pcon);
        parentDiv.appendChild(div);
        const childrenContainer = div.querySelector(".pcon-children");

        /* Render builders belonging to this PCON */
        /* We search the global builder list for those matching this pcon */
        const myBuilders = last_builder_list.filter(b => b.pcon === pcon.name);
        myBuilders.sort((a, b) => a.name.localeCompare(b.name));

        if (myBuilders.length > 0) {
            const table = document.createElement("table");
            table.className = "builders";
            const tbody = document.createElement("tbody");
            table.appendChild(tbody);
            myBuilders.forEach(b => {
                tbody.appendChild(createBuilderRow(b));
            });
            childrenContainer.appendChild(table);
        }

        /* Render child PCONs */
        pcon.children.forEach(child => {
            renderPcon(child, childrenContainer);
        });
    }

    roots.forEach(root => {
        renderPcon(root, container);
    });

    /* Render orphan builders (no pcon or unknown pcon) */
    const orphanBuilders = last_builder_list.filter(b => !b.pcon || !pcon_topology[b.pcon]);
    if (orphanBuilders.length > 0) {
        const orphanDiv = document.createElement("div");
        orphanDiv.className = "pcon-orphans";
        orphanDiv.innerHTML = "<div class='pcon-header'><b>Unmanaged Builders</b></div>";
        const childrenContainer = document.createElement("div");
        childrenContainer.className = "pcon-children";
        orphanDiv.appendChild(childrenContainer);

        const table = document.createElement("table");
        table.className = "builders";
        const tbody = document.createElement("tbody");
        table.appendChild(tbody);
        orphanBuilders.forEach(b => {
            tbody.appendChild(createBuilderRow(b));
        });
        childrenContainer.appendChild(table);

        container.appendChild(orphanDiv);
    }
}

// ... (rest of the file)

/* Inside onmessage switch */

			case "com.warmcat.sai.power_managed_builders":
				/* Update PCON topology */
				if (jso.power_controllers) {
					jso.power_controllers.forEach(pc => {
						pcon_topology[pc.name] = pc;
					});
					/* Trigger redraw if we have builders */
					const container = document.getElementById("sai_builders");
					if (container) renderPconHierarchy(container);
				}
				break;

 			case "com.warmcat.sai.builders":
				/* Update builder list */
				let platformsArray = (jso.platforms && Array.isArray(jso.platforms)) ? jso.platforms :
				                     (jso.builders && Array.isArray(jso.builders)) ? jso.builders : null;

				if (platformsArray) {
					last_builder_list = platformsArray;
					const container = document.getElementById("sai_builders");
					if (container) renderPconHierarchy(container);
				}
 				break;
