/*
 * Sai synchronised logs
 *
 * A pane that slides up from the bottom of the page, showing a run's task log
 * beside the fleet log of each host involved, see READMEs/README-logs.md.
 *
 * Every pane shows its lines either side of one instant, the line across the
 * middle of each.  The instant goes:
 *
 *  - line by line in a pane, with the wheel or the arrow keys there, and the
 *    other panes follow it in time
 *  - anywhere in the run, with the time bar on the right, which is the run's
 *    window, padded
 *
 * What's in a pane comes from sai-web a piece at a time, around the instant,
 * as it's needed: GET logs/pane.  Panes are filled from the left, only while
 * they can be seen, and a fetch that's overtaken by a jump elsewhere is
 * cancelled.
 *
 * Panes can be reordered by dragging their header, and sized by the splitter
 * after them, down to a narrow strip to put them out of the way.  The order
 * and sizes are kept in localStorage by what the pane is, eg, "the run's
 * builder", rather than which host it was this time.
 *
 * sai.js creates it with the helpers it shares, opens it from a line of a
 * task's log, and drives its splitters with its own.
 */

class SaiSync {
	constructor(opts) {
		this.ansi = opts.ansi;		/* text, state -> { html, newState } */
		this.san = opts.san;		/* text -> html-safe */

		this.allowed = false;
		this.ctx = null;		/* the answer to logs/sync */
		this.panes = [];
		this.T = 0;			/* the instant, UTC us */
		this.fetching = null;		/* the pane being filled */
		this.kick_timer = null;
		this.refresh_timer = null;
		this.gen = 0;			/* which open() we're on */

		this.layout = this.layout_load();

		this.el = document.getElementById("sai_sync");
		this.resizer = document.getElementById("sai_sync_resizer");
		if (!this.el)
			return;

		this.build();
	}

	/* ---- what we keep in localStorage ---- */

	static get LS_LAYOUT() { return "sai-sync-layout"; }
	static get LS_HEIGHT() { return "sai-sync-height"; }
	static get LS_OPEN() { return "sai-sync-open"; }

	layout_load() {
		var l = null;

		try {
			l = JSON.parse(localStorage.getItem(SaiSync.LS_LAYOUT));
		} catch (e) {}
		if (!l || !Array.isArray(l.order) || typeof l.widths !== "object" ||
		    !l.widths)
			l = { order: [], widths: {} };
		/* the width a folded pane unfolds to */
		if (typeof l.unfold !== "object" || !l.unfold)
			l.unfold = {};

		return l;
	}

	layout_save() {
		try {
			localStorage.setItem(SaiSync.LS_LAYOUT,
					     JSON.stringify(this.layout));
		} catch (e) {}
	}

	/* ---- the furniture ---- */

	build() {
		var bar = document.createElement("div"), body;

		bar.className = "sync-bar";
		bar.innerHTML =
			"<button type=\"button\" class=\"sync-toggle\" " +
				"title=\"Show or hide the synchronised logs\">" +
				"&#9650;</button>" +
			"<span class=\"sync-title\">Synchronised logs</span>" +
			"<span class=\"sync-run\"></span>" +
			"<span class=\"sync-instant\" title=\"The instant " +
				"across the middle of every pane, in UTC, and " +
				"how far into the run it is\"></span>" +
			"<span class=\"sync-hint\"></span>";
		this.el.appendChild(bar);

		body = document.createElement("div");
		body.className = "sync-body";
		body.innerHTML =
			"<div class=\"sync-panes\"></div>" +
			"<div class=\"sync-time\" title=\"The run's window: " +
				"drag to go anywhere in it\">" +
				"<div class=\"sync-time-run\"></div>" +
				"<div class=\"sync-time-thumb\"></div>" +
			"</div>";
		this.el.appendChild(body);

		this.bar = bar;
		this.body = body;
		this.toggle_btn = bar.querySelector(".sync-toggle");
		this.run_el = bar.querySelector(".sync-run");
		this.instant_el = bar.querySelector(".sync-instant");
		this.hint_el = bar.querySelector(".sync-hint");
		this.panes_el = body.querySelector(".sync-panes");
		this.time_el = body.querySelector(".sync-time");
		this.time_run_el = body.querySelector(".sync-time-run");
		this.thumb_el = body.querySelector(".sync-time-thumb");

		this.toggle_btn.addEventListener("click", () => this.toggle());
		bar.querySelector(".sync-title").addEventListener("click",
						() => this.toggle());

		this.time_bar_events();

		this.panes_el.addEventListener("scroll", () => this.kick());

		if (window.ResizeObserver)
			new ResizeObserver(() => this.relayout()).observe(this.body);

		this.el.addEventListener("keydown", (e) => {
			if (e.key === "Escape")
				this.show(false);
		});

		this.hint("Right-click a line of a task's log to see the " +
			  "fleet's logs at that moment");
	}

	hint(s) {
		this.hint_el.textContent = s || "";
	}

	/*
	 * Whether this login may query the fleet logs: sai-web says so to a
	 * logs/sync without a task.  Anything else, eg, the front end
	 * redirecting to a login, is a no.
	 */
	probe() {
		return fetch("logs/sync", { credentials: "same-origin",
					    cache: "no-store" })
			.then((r) => r.ok ? r.json() : null)
			.catch(() => null)
			.then((j) => {
				this.set_allowed(!!(j && j.allowed));
			});
	}

	set_allowed(a) {
		this.allowed = a;
		if (!this.el)
			return;
		this.el.classList.toggle("hidden", !a);
		if (!a)
			this.show(false);
		else
			try {
				if (localStorage.getItem(SaiSync.LS_OPEN) === "1")
					this.show(true);
			} catch (e) {}
	}

	is_open() {
		return this.el && this.el.classList.contains("open");
	}

	toggle() {
		this.show(!this.is_open());
	}

	show(open) {
		var h = null;

		if (!this.el)
			return;
		this.el.classList.toggle("open", !!open);
		this.resizer.classList.toggle("hidden", !open);
		this.toggle_btn.innerHTML = open ? "&#9660;" : "&#9650;";

		try {
			localStorage.setItem(SaiSync.LS_OPEN, open ? "1" : "0");
			h = localStorage.getItem(SaiSync.LS_HEIGHT);
		} catch (e) {}

		if (open) {
			this.el.style.flex = h ? h : "0 0 40vh";
			this.relayout();
			this.kick();
		} else
			this.el.style.flex = "";
	}

	/* sai.js' splitters drive the one above us */
	resize_begin() {
		var h0 = this.el.getBoundingClientRect().height;

		return {
			move: (dx, dy) => {
				this.el.style.flex = "0 0 " +
					Math.max(60, Math.round(h0 - dy)) + "px";
				this.relayout();
			},
			end: () => {
				try {
					localStorage.setItem(SaiSync.LS_HEIGHT,
							     this.el.style.flex);
				} catch (e) {}
				this.kick();
			}
		};
	}

	/* ...and the ones after each pane */
	split_begin(rz) {
		var p = this.panes.find((x) => x.split === rz), w0;

		if (!p)
			return null;
		w0 = p.el.getBoundingClientRect().width;

		return {
			move: (dx) => {
				this.pane_width(p, Math.round(w0 + dx));
			},
			end: () => {
				this.layout.widths[p.key] = p.width;
				this.layout_save();
				this.kick();
			}
		};
	}

	/* ---- a run to look at ---- */

	/*
	 * Show the run of task, at ts on its builder's clock, or its start
	 * without one.  run is the run's number, or -1 for the latest.
	 */
	open(task, run, ts) {
		var gen = ++this.gen,
		    q = "logs/sync?task=" + encodeURIComponent(task);

		if (run !== undefined && run !== null && run >= 0)
			q += "&run=" + run;
		if (ts !== undefined && ts !== null)
			q += "&ts=" + Math.max(0, Math.round(ts));

		this.show(true);
		this.hint("Finding out about the run...");

		return fetch(q, { credentials: "same-origin", cache: "no-store" })
			.then((r) => {
				if (!r.ok)
					throw new Error(r.status === 404 ?
						"That run hasn't started" :
						"sai-web said " + r.status);
				return r.json();
			})
			.then((j) => {
				if (gen !== this.gen)
					return;
				this.hint("");
				this.set_run(j);
				this.set_instant(j.at_us, null, true);
			})
			.catch((e) => {
				if (gen === this.gen)
					this.hint(e.message);
			});
	}

	set_run(j) {
		this.ctx = j;
		this.run_el.textContent = j.taskname + ", try " + (j.run + 1) +
					  " on " + j.builder;

		this.panes.forEach((p) => this.pane_drop(p));
		this.panes = [];
		this.panes_el.innerHTML = "";

		this.order(j.panes).forEach((d) => this.pane_add(d));

		if (this.refresh_timer)
			clearInterval(this.refresh_timer);
		this.refresh_timer = j.running ?
			setInterval(() => this.refresh(), 5000) : null;

		this.time_bar_render();
	}

	/*
	 * While the run is going, its window grows, and the panes at its end
	 * have more to show
	 */
	refresh() {
		var c = this.ctx, gen = this.gen;

		if (!c || !this.is_open())
			return;

		fetch("logs/sync?task=" + encodeURIComponent(c.task) + "&run=" +
		      c.run, { credentials: "same-origin", cache: "no-store" })
			.then((r) => r.ok ? r.json() : null)
			.catch(() => null)
			.then((j) => {
				if (!j || gen !== this.gen)
					return;

				c.end_us = j.end_us;
				c.to_us = j.to_us;
				c.running = j.running;
				if (!j.running && this.refresh_timer) {
					clearInterval(this.refresh_timer);
					this.refresh_timer = null;
				}

				/* hosts that have spoken up since */
				this.order(j.panes).forEach((d) => {
					if (!this.panes.find((p) => p.id === d.pane))
						this.pane_add(d);
				});

				this.panes.forEach((p) => {
					if (!p.more_after)
						p.stale = true;
				});

				this.time_bar_render();
				this.kick();
			});
	}

	/*
	 * What a pane is, for remembering where it goes and how wide it is:
	 * the run's builder is "builder", whichever host it was
	 */
	pane_key(d) {
		var c = this.ctx;

		if (d.kind === "task")
			return "task";
		if (d.kind === "sai-builder" && c &&
		    c.builder.indexOf(d.host + ".") === 0)
			return "builder";
		if (d.kind === "sai-server")
			return "sai-server";

		return d.kind + "/" + d.host;
	}

	/* the remembered order first, then the rest as sai-web had them */
	order(ds) {
		var o = this.layout.order;

		return ds.map((d, i) => ({ d: d, i: i,
					   at: o.indexOf(this.pane_key(d)) }))
			.sort((a, b) => {
				if (a.at >= 0 && b.at >= 0)
					return a.at - b.at;
				if (a.at >= 0 || b.at >= 0)
					return a.at >= 0 ? -1 : 1;
				return a.i - b.i;
			})
			.map((x) => x.d);
	}

	pane_title(d) {
		var c = this.ctx, key = this.pane_key(d);

		if (key === "task")
			return c.taskname + " (task log)";
		if (key === "builder")
			return "builder " + d.host;

		return d.kind + " " + d.host;
	}

	pane_add(d) {
		var p = {
			id: d.pane, d: d, key: this.pane_key(d),
			rows: [], more_before: false, more_after: false,
			loaded: false, stale: false, ctl: null,
			k: -1, wheel: 0
		}, w = this.layout.widths[p.key], s;

		p.el = document.createElement("div");
		p.el.className = "sync-pane sync-kind-" + d.kind;
		p.el.innerHTML =
			"<div class=\"sync-pane-head\" draggable=\"true\">" +
				"<span class=\"sync-pane-name\"></span>" +
				"<span class=\"sync-pane-delta\"></span>" +
			"</div>" +
			"<div class=\"sync-pane-view\" tabindex=\"0\">" +
				"<div class=\"sync-pane-scroll\">" +
					"<div class=\"sync-pane-text\"></div>" +
				"</div>" +
				"<div class=\"sync-pane-cursor\"></div>" +
				"<div class=\"sync-pane-state\"></div>" +
			"</div>";

		p.head = p.el.querySelector(".sync-pane-head");
		p.delta_el = p.el.querySelector(".sync-pane-delta");
		p.view = p.el.querySelector(".sync-pane-view");
		p.scroll = p.el.querySelector(".sync-pane-scroll");
		p.text = p.el.querySelector(".sync-pane-text");
		p.state_el = p.el.querySelector(".sync-pane-state");

		p.el.querySelector(".sync-pane-name").textContent =
							this.pane_title(d);
		s = this.pane_title(d);
		if (d.kind !== "task") {
			s += "\nits clock was " + d.lag_ms + "ms behind " +
			     "sai-server's, or its lines waited that long";
			if (d.insts > 1)
				s += "\n" + d.insts + " instances of it in the " +
				     "window, eg, it restarted";
		}
		p.head.title = s + "\ndrag to move it, double-click to " +
			       "fold or unfold it";

		p.split = document.createElement("div");
		p.split.className = "sync-split";

		this.panes_el.appendChild(p.el);
		this.panes_el.appendChild(p.split);
		this.panes.push(p);

		this.pane_width(p, w ? w : (d.kind === "task" ? 560 : 420));
		this.pane_events(p);
		this.pane_text_pad(p);
	}

	pane_drop(p) {
		if (p.ctl)
			p.ctl.abort();
		p.ctl = null;
	}

	/* narrow enough and it's folded out of the way */
	pane_width(p, w) {
		p.width = Math.max(14, w);
		p.el.style.flex = "0 0 " + p.width + "px";
		p.el.classList.toggle("folded", p.width < 48);
	}

	pane_events(p) {
		p.view.addEventListener("wheel", (e) => {
			var n;

			e.preventDefault();
			/*
			 * A step a line: a wheel's notch is usually 3 lines,
			 * or about 100px, and a touchpad's is a little at a
			 * time
			 */
			p.wheel += e.deltaMode === 1 ? e.deltaY * 33 :
				   (e.deltaMode === 2 ? e.deltaY * 660 :
				    e.deltaY);
			n = Math.trunc(p.wheel / 33);
			if (!n)
				return;
			p.wheel -= n * 33;
			this.step(p, n);
		}, { passive: false });

		p.view.addEventListener("keydown", (e) => {
			var n = { ArrowDown: 1, ArrowUp: -1, PageDown: 20,
				  PageUp: -20 }[e.key];

			if (n) {
				e.preventDefault();
				this.step(p, n);
			}
		});

		/* a finger drags the instant along the pane */
		p.view.addEventListener("touchstart", (e) => {
			if (e.touches.length === 1)
				p.touch_y = e.touches[0].clientY;
		}, { passive: true });
		p.view.addEventListener("touchmove", (e) => {
			var y, n;

			if (e.touches.length !== 1 || p.touch_y === undefined)
				return;
			e.preventDefault();
			y = e.touches[0].clientY;
			n = Math.trunc((p.touch_y - y) / 16);
			if (n) {
				p.touch_y -= n * 16;
				this.step(p, n);
			}
		}, { passive: false });

		p.head.addEventListener("dblclick", () => {
			var w = p.width < 48 ? (this.layout.unfold[p.key] ||
					      420) : 14;

			if (p.width >= 48)
				this.layout.unfold[p.key] = p.width;
			this.pane_width(p, w);
			this.layout.widths[p.key] = p.width;
			this.layout_save();
			this.kick();
		});

		/* reordering, by dragging the header onto another */
		p.head.addEventListener("dragstart", (e) => {
			e.dataTransfer.setData("text/plain", p.id);
			e.dataTransfer.effectAllowed = "move";
			this.dragging = p;
		});
		p.head.addEventListener("dragend", () => {
			this.dragging = null;
			this.panes.forEach((x) =>
				x.el.classList.remove("drop-before",
						      "drop-after"));
		});
		p.el.addEventListener("dragover", (e) => {
			var r, after;

			if (!this.dragging || this.dragging === p)
				return;
			e.preventDefault();
			r = p.el.getBoundingClientRect();
			after = e.clientX > r.left + r.width / 2;
			p.el.classList.toggle("drop-after", after);
			p.el.classList.toggle("drop-before", !after);
		});
		p.el.addEventListener("dragleave", () => {
			p.el.classList.remove("drop-before", "drop-after");
		});
		p.el.addEventListener("drop", (e) => {
			var from = this.dragging, after;

			e.preventDefault();
			after = p.el.classList.contains("drop-after");
			p.el.classList.remove("drop-before", "drop-after");
			if (from && from !== p)
				this.pane_move(from, p, after);
		});
	}

	pane_move(p, to, after) {
		var ref = after ? to.split.nextSibling : to.el;

		this.panes_el.insertBefore(p.el, ref);
		this.panes_el.insertBefore(p.split, ref);

		this.panes.splice(this.panes.indexOf(p), 1);
		this.panes.splice(this.panes.indexOf(to) + (after ? 1 : 0), 0, p);

		/* the panes we have now, in front of any we don't */
		this.layout.order = this.panes.map((x) => x.key).concat(
			this.layout.order.filter((k) =>
				!this.panes.find((x) => x.key === k)));
		this.layout_save();
		this.kick();
	}

	/*
	 * Room above the first line and below the last, so either can sit on
	 * the instant line in the middle
	 */
	pane_text_pad(p) {
		var h = Math.round(p.view.clientHeight / 2) + "px";

		p.text.style.paddingTop = h;
		p.text.style.paddingBottom = h;
	}

	relayout() {
		this.panes.forEach((p) => {
			this.pane_text_pad(p);
			this.pane_position(p);
		});
		this.time_bar_render();
	}

	/* ---- the instant ---- */

	/*
	 * origin is the pane it was stepped in, if any.  A jump is a move
	 * that leaves what the panes have behind, so what they're fetching
	 * is no use.
	 */
	set_instant(T, origin, jump) {
		var c = this.ctx;

		if (!c)
			return;
		this.T = Math.min(Math.max(T, c.from_us), c.to_us);

		if (jump)
			this.panes.forEach((p) => {
				if (p.ctl && p !== origin) {
					p.ctl.abort();
					p.ctl = null;
				}
			});

		this.instant_render();
		this.panes.forEach((p) => this.pane_position(p));
		this.time_bar_render();
		this.kick(jump);
	}

	instant_render() {
		var c = this.ctx, T = this.T, d, rel, us;

		if (!c)
			return;

		d = new Date(Math.floor(T / 1000));
		us = String(T % 1000).padStart(3, "0");
		rel = (T - c.start_us) / 1000000;
		this.instant_el.textContent =
			d.toISOString().replace("T", " ").replace("Z", us) +
			" UTC, " + (rel < 0 ? "-" : "+") +
			Math.abs(rel).toFixed(6) + "s";
	}

	/* the pane's last row at or before the instant, or -1 */
	pane_k(p) {
		var lo = 0, hi = p.rows.length - 1, k = -1, m;

		while (lo <= hi) {
			m = (lo + hi) >> 1;
			if (p.rows[m].t <= this.T) {
				k = m;
				lo = m + 1;
			} else
				hi = m - 1;
		}

		return k;
	}

	/*
	 * n lines on in pane p, or back, by the times of its rows: rows that
	 * came in together, eg, parts of what a builder read at once, are one
	 * step.  At the end of what the pane has, it fetches more and the step
	 * waits for it.
	 */
	step(p, n) {
		var r = p.rows, k = this.pane_k(p), t;

		if (!r.length)
			return;

		if (n > 0) {
			if (k < 0) {
				k = 0;
				n--;
			}
			while (n-- > 0) {
				t = r[k].t;
				while (k < r.length - 1 && r[k].t === t)
					k++;
				if (r[k].t === t)
					break;	/* that's all it has */
			}
		} else {
			if (k < 0)
				return;
			/* to the first row of the instant's time */
			while (k > 0 && r[k - 1].t === r[k].t)
				k--;
			while (n++ < 0 && k > 0) {
				k--;
				while (k > 0 && r[k - 1].t === r[k].t)
					k--;
			}
		}

		this.set_instant(r[k].t, p, false);
	}

	/* the instant on the line in the middle, and its rows marked */
	pane_position(p) {
		var k, k0, els = p.els, top, d;

		if (!els)
			return;

		els.forEach((e) => e.classList.remove("sync-now"));
		p.delta_el.textContent = "";

		k = this.pane_k(p);
		p.k = k;
		if (k < 0) {
			/* before its first row: that's just after the line */
			top = els.length ? els[0].offsetTop - p.line_h : 0;
		} else {
			for (k0 = k; k0 > 0 && p.rows[k0 - 1].t === p.rows[k].t;
			     k0--)
				;
			for (var i = k0; i <= k; i++)
				els[i].classList.add("sync-now");
			top = els[k].offsetTop;
			d = (p.rows[k].t - this.T) / 1000000;
			if (d)
				p.delta_el.textContent = d.toFixed(3) + "s";
		}

		/* the middle of its line on the line */
		p.scroll.scrollTop = Math.max(0, top + Math.round(p.line_h / 2) -
				Math.round(p.view.clientHeight / 2));
	}

	/* ---- filling the panes ---- */

	/*
	 * Whether a pane needs more around the instant: it has nothing yet,
	 * the instant is past what it has, or near the end of it with more
	 * there
	 */
	pane_needs(p) {
		var k, margin = 40;

		/* it failed here, until the instant moves */
		if (p.failed_at === this.T)
			return false;
		if (!p.loaded || p.stale)
			return true;

		k = this.pane_k(p);
		if (p.more_before && k < margin)
			return true;
		if (p.more_after && k > p.rows.length - 1 - margin)
			return true;

		return false;
	}

	/* folded, or scrolled out of sight, it can wait */
	pane_visible(p) {
		var r = p.el.getBoundingClientRect(),
		    v = this.panes_el.getBoundingClientRect();

		return p.width >= 48 && r.right > v.left && r.left < v.right;
	}

	/*
	 * Fill the panes soon.  After a jump, wait until it's stopped moving,
	 * eg, the time bar being dragged, so we only fetch where it ends up.
	 */
	kick(jump) {
		if (this.kick_timer) {
			if (!jump)
				return;
			clearTimeout(this.kick_timer);
		}
		this.kick_timer = setTimeout(() => {
			this.kick_timer = null;
			this.fill();
		}, jump ? 150 : 60);
	}

	/* one at a time, from the left */
	fill() {
		var p;

		if (this.fetching || !this.is_open() || !this.ctx)
			return;

		p = this.panes.find((x) => this.pane_visible(x) &&
					   this.pane_needs(x));
		if (!p)
			return;

		this.fetching = p;
		this.pane_fetch(p).finally(() => {
			this.fetching = null;
			this.fill();
		});
	}

	pane_fetch(p) {
		var c = this.ctx, at = this.T, q, gen = this.gen;

		q = "logs/pane?pane=" + encodeURIComponent(p.id) + "&at=" + at +
		    "&before=150&after=150";
		if (p.d.kind === "task")
			q += "&task=" + encodeURIComponent(c.task) + "&run=" +
			     c.run;

		p.ctl = new AbortController();
		if (!p.loaded)
			p.state_el.textContent = "...";

		return fetch(q, { credentials: "same-origin", cache: "no-store",
				  signal: p.ctl.signal })
			.then((r) => {
				if (!r.ok)
					throw new Error("sai-web said " +
							r.status);
				return r.json();
			})
			.then((j) => {
				if (gen !== this.gen)
					return;
				p.rows = j.before.concat(j.after);
				p.more_before = j.more_before;
				p.more_after = j.more_after;
				p.loaded = true;
				p.stale = false;
				p.failed_at = undefined;
				p.state_el.textContent = p.rows.length ? "" :
						"nothing around then";
				this.pane_render(p);
				this.pane_position(p);
			})
			.catch((e) => {
				if (e.name === "AbortError")
					return;
				p.failed_at = at;
				p.state_el.textContent = e.message;
			})
			.finally(() => {
				p.ctl = null;
			});
	}

	/* ---- drawing a pane ---- */

	static b64_text(b) {
		var bin, bytes, i;

		try {
			bin = atob(b);
		} catch (e) {
			return "";
		}
		bytes = new Uint8Array(bin.length);
		for (i = 0; i < bin.length; i++)
			bytes[i] = bin.charCodeAt(i);

		return new TextDecoder("utf-8").decode(bytes);
	}

	static utc_hms(t) {
		return new Date(Math.floor(t / 1000)).toISOString()
			.substring(11, 23);
	}

	pane_render(p) {
		var h = [], c = this.ctx;

		if (p.d.kind === "task") {
			var st = {}, sol = true;

			p.rows.forEach((r) => {
				var s = SaiSync.b64_text(r.b), a, cls;

				/* a line starts with how far into the run */
				a = sol ? "<span class=\"sync-t\">+" +
					((r.t - c.start_us) / 1000000).toFixed(3) +
					" </span>" : "";
				sol = /[\r\n]$/.test(s);
				s = s.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
				st = this.ansi(s, st);
				cls = { 2: "stderr", 3: "saibuild", 4: "tty0" }
					[r.c] || (r.c === 1 ? "" : "tty1");
				h.push("<span class=\"sync-row " + cls + "\">" + a +
				       st.html + "</span>");
				st = st.newState;
			});
		} else {
			var src = null;

			p.rows.forEach((r) => {
				var cls = r.l === 1 ? " sync-err" :
					  (r.l === 2 ? " sync-warn" : "");

				if (r.h && r.h === c.hash)
					cls += " sync-ours";
				if (src !== null && r.s !== src)
					h.push("<div class=\"sync-inst\">" +
					       "another instance of " +
					       this.san(p.d.host) + "</div>");
				src = r.s;
				h.push("<div class=\"sync-row" + cls + "\">" +
				       "<span class=\"sync-t\">" +
				       SaiSync.utc_hms(r.t) + " </span>" +
				       this.san(r.x) + "</div>");
			});
		}

		p.text.innerHTML = h.join("");
		p.els = Array.from(p.text.querySelectorAll(".sync-row"));
		p.line_h = parseFloat(getComputedStyle(p.text).lineHeight) || 14;
	}

	/* ---- the time bar: the run's window, top to bottom ---- */

	time_y(t) {
		var c = this.ctx, h = this.time_el.clientHeight;

		return Math.round((t - c.from_us) / (c.to_us - c.from_us) * h);
	}

	time_bar_render() {
		var c = this.ctx, y0, y1;

		if (!c || !this.time_el.clientHeight)
			return;

		y0 = this.time_y(c.start_us);
		y1 = this.time_y(c.end_us);
		this.time_run_el.style.top = y0 + "px";
		this.time_run_el.style.height = Math.max(2, y1 - y0) + "px";
		this.time_run_el.classList.toggle("running", !!c.running);
		this.thumb_el.style.top = (this.time_y(this.T) - 1) + "px";
	}

	time_bar_events() {
		var to_t = (e) => {
			var c = this.ctx, r = this.time_el.getBoundingClientRect();

			return c.from_us + Math.round(Math.min(1, Math.max(0,
				(e.clientY - r.top) / r.height)) *
				(c.to_us - c.from_us));
		};

		this.time_el.addEventListener("pointerdown", (e) => {
			if (!this.ctx || e.button !== 0)
				return;
			e.preventDefault();
			this.time_el.setPointerCapture(e.pointerId);
			this.time_el.classList.add("dragging");
			this.set_instant(to_t(e), null, true);
		});
		this.time_el.addEventListener("pointermove", (e) => {
			if (this.time_el.hasPointerCapture(e.pointerId))
				this.set_instant(to_t(e), null, true);
		});
		this.time_el.addEventListener("pointerup", (e) => {
			if (this.time_el.hasPointerCapture(e.pointerId))
				this.time_el.releasePointerCapture(e.pointerId);
			this.time_el.classList.remove("dragging");
		});
	}
}
