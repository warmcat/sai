const SAI_JS_API_VERSION = 3;

(function() {

/*
 * We display untrusted stuff in html context... reject anything
 * that has HTML stuff in it
 */

/* http://i18njs.com/ this from http://i18njs.com/js/i18n.js */
(function() {
  var Translator, i18n, translator,
    __bind = function(fn, me){ return function(){ return fn.apply(me, arguments); }; };

  Translator = (function() {
    function Translator() {
      this.translate = __bind(this.translate, this);      this.data = {
        values: {},
        contexts: []
      };
      this.globalContext = {};
    }

    Translator.prototype.translate = function(text, defaultNumOrFormatting,
			numOrFormattingOrContext, formattingOrContext, context) {
      var defaultText, formatting, isObject, num;

      if (context == null) {
        context = this.globalContext;
      }
      isObject = function(obj) {
        var type;

        type = typeof obj;
        return type === "function" || type === "object" && !!obj;
      };
      if (isObject(defaultNumOrFormatting)) {
        defaultText = null;
        num = null;
        formatting = defaultNumOrFormatting;
        context = numOrFormattingOrContext || this.globalContext;
      } else {
        if (typeof defaultNumOrFormatting === "number") {
          defaultText = null;
          num = defaultNumOrFormatting;
          formatting = numOrFormattingOrContext;
          context = formattingOrContext || this.globalContext;
        } else {
          defaultText = defaultNumOrFormatting;
          if (typeof numOrFormattingOrContext === "number") {
            num = numOrFormattingOrContext;
            formatting = formattingOrContext;
            context = context;
          } else {
            num = null;
            formatting = numOrFormattingOrContext;
            context = formattingOrContext || this.globalContext;
          }
        }
      }
      if (isObject(text)) {
        if (isObject(text['i18n'])) {
          text = text['i18n'];
        }
        return this.translateHash(text, context);
      } else {
        return this.translateText(text, num, formatting, context, defaultText);
      }
    };

    Translator.prototype.add = function(d) {
      var c, v, _i, _len, _ref, _ref1, _results;

      if ((d.values != null)) {
        _ref = d.values;
        var k;
        for (k in _ref) {
	  if ({}.hasOwnProperty.call(_ref, k)) {
          v = _ref[k];
          this.data.values[k] = v;
	  }
        }
      }
      if ((d.contexts != null)) {
        _ref1 = d.contexts;
        _results = [];
        for (_i = 0, _len = _ref1.length; _i < _len; _i++) {
          c = _ref1[_i];
          _results.push(this.data.contexts.push(c));
        }
        return _results;
      }
    };

    Translator.prototype.setContext = function(key, value) {
      return this.globalContext[key] = value;
    };

    Translator.prototype.clearContext = function(key) {
      return this.lobalContext[key] = null;
    };

    Translator.prototype.reset = function() {
      this.data = {
        values: {},
        contexts: []
      };
      return this.globalContext = {};
    };

    Translator.prototype.resetData = function() {
      return this.data = {
        values: {},
        contexts: []
      };
    };

    Translator.prototype.resetContext = function() {
      return this.globalContext = {};
    };

    Translator.prototype.translateHash = function(hash, context) {
      var k, v;

      for (k in hash) {
	  if ({}.hasOwnProperty.call(hash, k)) {
	        v = hash[k];
	        if (typeof v === "string") {
	          hash[k] = this.translateText(v, null, null, context);
	        }
	  }
      }
      return hash;
    };

    Translator.prototype.translateText = function(text, num, formatting,
						context, defaultText) {
      var contextData, result;

      if (context == null) {
        context = this.globalContext;
      }
      if (this.data == null) {
        return this.useOriginalText(defaultText || text, num, formatting);
      }
      contextData = this.getContextData(this.data, context);
      if (contextData != null) {
        result = this.findTranslation(text, num, formatting, contextData.values,
					defaultText);
      }
      if (result == null) {
        result = this.findTranslation(text, num, formatting, this.data.values,
					defaultText);
      }
      if (result == null) {
        return this.useOriginalText(defaultText || text, num, formatting);
      }
      return result;
    };

    Translator.prototype.findTranslation = function(text, num, formatting, data) {
      var result, triple, value, _i, _len;

      value = data[text];
      if (value == null) {
        return null;
      }
      if (num == null) {
        if (typeof value === "string") {
          return this.applyFormatting(value, num, formatting);
        }
      } else {
        if (value instanceof Array || value.length) {
          for (_i = 0, _len = value.length; _i < _len; _i++) {
            triple = value[_i];
            if ((num >= triple[0] || triple[0] === null) &&
                (num <= triple[1] || triple[1] === null)) {
              result = this.applyFormatting(triple[2].replace("-%n",
						String(-num)), num, formatting);
              return this.applyFormatting(result.replace("%n",
						String(num)), num, formatting);
            }
          }
        }
      }
      return null;
    };

    Translator.prototype.getContextData = function(data, context) {
      var c, equal, key, value, _i, _len, _ref, _ref1;

      if (data.contexts == null) {
        return null;
      }
      _ref = data.contexts;
      for (_i = 0, _len = _ref.length; _i < _len; _i++) {
        c = _ref[_i];
        equal = true;
        _ref1 = c.matches;
        for (key in _ref1) {
		if ({}.hasOwnProperty.call(_ref1, key)) {
			value = _ref1[key];
			equal = equal && value === context[key];
		}
        }
        if (equal) {
          return c;
        }
      }
      return null;
    };

    Translator.prototype.useOriginalText = function(text, num, formatting) {
      if (num == null) {
        return this.applyFormatting(text, num, formatting);
      }
      return this.applyFormatting(text.replace("%n", String(num)),
					num, formatting);
    };

    Translator.prototype.applyFormatting = function(text, num, formatting) {
      var ind, regex;

      for (ind in formatting) {
	  if ({}.hasOwnProperty.call(formatting, ind)) {
	        regex = new RegExp("%{" + ind + "}", "g");
	        text = text.replace(regex, formatting[ind]);
	  }
      }
      return text;
    };

    return Translator;

  })();

  translator = new Translator();

  i18n = translator.translate;

  i18n.translator = translator;

  i18n.create = function(data) {
    var trans;

    trans = new Translator();
    if (data != null) {
      trans.add(data);
    }
    trans.translate.create = i18n.create;
    return trans.translate;
  };

  (typeof module !== "undefined" && module !== null ? module.exports = i18n : void 0) ||
	(this.i18n = i18n);

}.call(this));

var lang_ja = "{" +
  "\"values\":{" +
    "\"Summary\": \"概要\"," +
    "\"Log\": \"ログ\"," +
    "\"Tree\": \"木構造\"," +
    "\"Blame\": \"責任\"," +
    "\"Copy Lines\": \"コピーライン\"," +
    "\"Copy Link\": \"リンクをコピーする\"," +
    "\"View Blame\": \"責任がある\"," +
    "\"Remove Blame\": \"責任を取り除く\"," +
    "\"Mode\": \"モード\"," +
    "\"Size\": \"サイズ\"," +
    "\"Name\": \"名\"," +
    "\"s\": \"秒\"," +
    "\"m\": \"分\"," +
    "\"h\": \"時間\"," +
    "\" days\": \"日々\"," +
	"\" weeks\": \"週\"," +
	"\" months\": \"数ヶ月\"," +
	"\" years\": \"年\"," +
	"\"Branch Snapshot\": \"ブランチスナップショット\"," +
	"\"Tag Snapshot\": \"タグスナップショット\"," +
	"\"Commit Snapshot\": \"スナップショットをコミットする\"," +
	"\"Description\": \"説明\"," +
	"\"Owner\": \"オーナー\"," +
	"\"Branch\": \"ブランチ\"," +
	"\"Tag\": \"タグ\"," +
	"\"Author\": \"著者\"," +
	"\"Age\": \"年齢\"," +
	"\"Page fetched\": \"ページを取得した\"," +
	"\"creation time\": \"作成時間\"," +
	"\"created\": \"作成した\"," +
	"\"ago\": \"前\"," +
	"\"Message\": \"メッセージ\"," +
	"\"Download\": \"ダウンロード\"," +
	"\"root\": \"ルート\"," +
	"\"Committer\": \"コミッター\"," +
	"\"Raw Patch\": \"生パッチ\"," +
	"\"Page fetched %{pf} ago, creation time: %{ct}ms " +
	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
	"\"%{pf}間前に取得されたページ, 作成にかかった時間: %{ct}ms " +
	   "(vhost etag キャッシュヒット: %{ve}%, キャッシュヒット: %{ch}%)\"," +
	"\"Created %{pf} ago, creation time: %{ct}ms \":\"" +
	   "%{pf}間前に作成されました, 作成にかかった時間: %{ct}ms\"" +
  "}}";

var lang_zht = "{" +
"\"values\":{" +
  "\"Summary\": \"概要\"," +
  "\"Log\": \"日誌\"," +
  "\"Tree\": \"樹\"," +
  "\"Blame\": \"責怪\"," +
  "\"Copy Lines\": \"複製線\"," +
  "\"Copy Link\": \"複製鏈接\"," +
  "\"View Blame\": \"看責怪\"," +
  "\"Remove Blame\": \"刪除責怪\"," +
  "\"Mode\": \"模式\"," +
  "\"Size\": \"尺寸\"," +
  "\"Name\": \"名稱\"," +
  "\"s\": \"秒\"," +
  "\"m\": \"分鐘\"," +
  "\"h\": \"小時\"," +
  "\" days\": \"天\"," +
  "\" weeks\": \"週\"," +
  "\" months\": \"個月\"," +
  "\" years\": \"年份\"," +
  "\"Branch Snapshot\": \"科快照\"," +
  "\"Tag Snapshot\": \"标签快照\"," +
  "\"Commit Snapshot\": \"提交快照\"," +
  "\"Description\": \"描述\"," +
  "\"Owner\": \"所有者\"," +
  "\"Branch\": \"科\"," +
  "\"Tag\": \"標籤\"," +
  "\"Author\": \"作者\"," +
  "\"Age\": \"年齡\"," +
  "\"Page fetched\": \"頁面已獲取\"," +
  "\"creation time\": \"創作時間\"," +
  "\"created\": \"創建\"," +
  "\"ago\": \"前\"," +
  "\"Message\": \"信息\"," +
  "\"Download\": \"下載\"," +
  "\"root\": \"根源\"," +
  "\"Committer\": \"提交者\"," +
  "\"Raw Patch\": \"原始補丁\"," +
  "\"Page fetched %{pf} ago, creation time: %{ct}ms " +
	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
	"\"頁面%{pf}前獲取, 創作時間: %{ct}ms " +
	   "(vhost etag 緩存命中: %{ve}%, 緩存命中: %{ch}%)\"," +
  "\"Created %{pf} ago, creation time: %{ct}ms \":\"" +
	"%{pf}前創建, 創作時間: %{ct}ms \"" +
"}}";

var lang_zhs = "{" +
"\"values\":{" +
  "\"Summary\": \"概要\"," +
  "\"Log\": \"日志\"," +
  "\"Tree\": \"木\"," +
  "\"Blame\": \"归咎\"," +
  "\"Copy Lines\": \"复制线\"," +
  "\"Copy Link\": \"复制链接\"," +
  "\"View Blame\": \"看责备\"," +
  "\"Remove Blame\": \"删除责备\"," +
  "\"Mode\": \"模式\"," +
  "\"Size\": \"尺寸\"," +
  "\"Name\": \"名称\"," +
  "\"s\": \"秒\"," +
  "\"m\": \"分钟\"," +
  "\"h\": \"小时\"," +
  "\" days\": \"天\"," +
  "\" weeks\": \"周\"," +
  "\" months\": \"个月\"," +
  "\" years\": \"年份\"," +
  "\"Branch Snapshot\": \"科快照\"," +
  "\"Tag Snapshot\": \"标签快照\"," +
  "\"Commit Snapshot\": \"提交快照\"," +
  "\"Description\": \"描述\"," +
  "\"Owner\": \"所有者\"," +
  "\"Branch\": \"科\"," +
  "\"Tag\": \"标签\"," +
  "\"Author\": \"作者\"," +
  "\"Age\": \"年龄\"," +
  "\"Page fetched\": \"页面已获取\"," +
  "\"creation time\": \"创作时间\"," +
  "\"created\": \"创建\"," +
  "\"ago\": \"前\"," +
  "\"Message\": \"信息\"," +
  "\"Download\": \"下载\"," +
  "\"root\": \"根源\"," +
  "\"Committer\": \"提交者\"," +
  "\"Raw Patch\": \"原始补丁\"," +
  "\"Page fetched %{pf} ago, creation time: %{ct}ms " +
	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
	"\"页面%{pf}前获取, 创作时间: %{ct}ms " +
	   "(vhost etag 缓存命中: %{ve}%, 缓存命中: %{ch}%)\"," +
   "\"Created %{pf} ago, creation time: %{ct}ms \":" +
		"\"%{pf}前创建, 创作时间: %{ct}ms \"" +
"}}";

const SaiAuthState = {
	NOT_LOGGED_IN: 0,
	LOGGED_IN_NO_GRANT: 1,
	LOGGED_IN_GRANT_USER: 2,   // < :2
	LOGGED_IN_GRANT_ADMIN: 3   // >= :2
};

var logs = "", redpend = 0, gitohashi_integ = 0, authd = 0, auth_is_admin = 0, auth_grant_level = -1, auth_state = SaiAuthState.NOT_LOGGED_IN, exptimer, auth_user = "",
active_terminals = {};
	logAnsiState = {}, logs_pending = "", lines_pending = "", times_pending = "",
	ongoing_task_activities = {}, last_log_timestamp = 0, spreadsheet_data_cache = {}, loadreport_data_cache = {},
	watcher_services = [],
	fadingTasks = new Map();

var segment_stack = [];
var seg_counter = 0;

window.addEventListener('beforeunload', () => {
	for (const task_uuid in active_terminals) {
		const closeMsg = {
			schema: "com.warmcat.sai.closeshell",
			task_uuid: task_uuid
		};
		if (typeof sai !== 'undefined' && sai && sai.readyState === WebSocket.OPEN) {
			sai.send(JSON.stringify(closeMsg));
		}
	}
});

/* Global caches for reconcilation */
var pcon_topology = {};
var pcon_energy_cache = {};
var last_builder_list = [];
var current_overview_offset = 0;

window.change_page = function(new_offset) {
	current_overview_offset = new_offset;
	sai.send("{\"schema\":" +
		"\"com.warmcat.sai.taskinfo\", \"js_api_version\": " + SAI_JS_API_VERSION +
		", \"offset\": " + current_overview_offset + "}");
};

function createPconDiv(pcon) {
    const pconDiv = document.createElement("div");
    pconDiv.className = "pcon";
    pconDiv.id = "pcon-" + pcon.name;
    pconDiv.style.marginLeft = "10px";
    pconDiv.style.borderLeft = "1px solid #ccc";
    pconDiv.style.paddingLeft = "5px";

    const header = document.createElement("div");
    header.className = "pcon-header";

    let stateClass = (pcon.on === 1) ? "pcon-on" : "pcon-off";
    let type = pcon.type ? `(${pcon.type})` : "";

    header.innerHTML = `<span class="${stateClass}">&#x23FB;</span> <b>${hsanitize(pcon.name)}</b> <span class="pcon-type">${hsanitize(type)}</span>`;
    pconDiv.appendChild(header);

    const childrenDiv = document.createElement("div");
    childrenDiv.className = "pcon-children";
    pconDiv.appendChild(childrenDiv);

    return pconDiv;
}

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

function update_task_activities() {
	for (const uuid in ongoing_task_activities) {
		const el = document.getElementById("taskstate_" + uuid);
		if (el) {
			const cat = ongoing_task_activities[uuid];
			el.classList.remove("activity-1", "activity-2", "activity-3");
			if (cat > 0) {
				el.classList.add("activity-" + cat);
			}
		}
	}
}

function expiry()
{
	location.reload();
}

function san(s)
{
	var table = {
		'<': 'lt',
		'>': 'gt',
		'"': 'quot',
		'\'': 'apos',
		'&': 'amp'
	};

	return s.toString().replace(/[<>"'&]/g, function(chr) {
		return '&' + table[chr] + ';';
	});
}

function humanize(s)
{
	var i = parseInt(s, 10);

	if (i >= (1024 * 1024 * 1024))
		return (i / (1024 * 1024 * 1024)).toFixed(3) + "Gi";

	if (i >= (1024 * 1024))
		return (i / (1024 * 1024)).toFixed(3) + "Mi";

	if (i > 1024)
		return (i / 1024).toFixed(3) + "Ki";

	return s;
}

function flush_segments() {
	var target_id = segment_stack.length > 0 ? segment_stack[segment_stack.length - 1].id : "root";
	var c_idx = segment_stack.length > 0 ? (segment_stack[segment_stack.length - 1].chunk_index || 0) : 0;
	
	var logs_dom = target_id === "root" ? "logs" : ("dlogs-" + target_id + "-" + c_idx);
	var lines_dom = target_id === "root" ? "dlogsn" : ("dlogsn-" + target_id + "-" + c_idx);
	var times_dom = target_id === "root" ? "dlogst" : ("dlogst-" + target_id + "-" + c_idx);
	
	if (document.getElementById(logs_dom) && logs_pending) {
		document.getElementById(logs_dom).insertAdjacentHTML('beforeend', logs_pending);
	}
	if (document.getElementById(lines_dom) && lines_pending) {
		document.getElementById(lines_dom).insertAdjacentHTML('beforeend', lines_pending);
	}
	if (document.getElementById(times_dom) && times_pending) {
		document.getElementById(times_dom).insertAdjacentHTML('beforeend', times_pending);
	}
	
	logs_pending = lines_pending = times_pending = "";
	
	if (target_id !== "root") {
		var seg = segment_stack[segment_stack.length - 1];
		var ehdr = document.getElementById("hdr-seg-" + target_id);
		if (ehdr) {
			ehdr.querySelector('.seg-lines').innerText = seg.lines_count;
			var errSpan = ehdr.querySelector('.seg-errors');
			
			if (seg.error_count > 0) {
				errSpan.innerText = seg.error_count + " errors";
				ehdr.classList.add("has-error");
				errSpan.parentElement.classList.add("seg-errors-bold");
			} else if (seg.warning_count > 0) {
				ehdr.classList.add("has-warning");
			}
		}
	}
}

function append_chunk_table(id, chunk_index, target_dom) {
	var html = '<table><tr>' +
		'<td class="atop"><div class="dlogsn" id="dlogsn-' + id + '-' + chunk_index + '"></div></td>' +
		'<td class="atop"><div class="dlogst" id="dlogst-' + id + '-' + chunk_index + '"></div></td>' +
		'<td class="atop"><div class="dlogs"><span class="nowrap" id="dlogs-' + id + '-' + chunk_index + '"></span></div></td>' +
	'</tr></table>';
	if (target_dom) target_dom.insertAdjacentHTML('beforeend', html);
}

function push_segment(title, default_folded) {
	flush_segments();
	seg_counter++;
	var id = seg_counter;
	
	var seg = { id: id, title: title, lines_count: 0, error_count: 0, warning_count: 0, folded: default_folded, chunk_index: 0, auto_unfolded: false, user_toggled: false };
	
	var parent_id = segment_stack.length > 0 ? segment_stack[segment_stack.length - 1].id : "root";
	// Append to root's dlogs container OR the parent segment's BODY container
	var parent_logs_dom = parent_id === "root" ? "dlogs" : ("seg-" + parent_id);
	var parent_dom = document.getElementById(parent_logs_dom);
	
	if (parent_dom) {
		var icon = default_folded ? "▶" : "▼";
		var hideClass = default_folded ? " hide" : "";
		
		var clean_title = title.replace(/^[\s\S]*?(?:>|&gt;)saib(?:>|&gt;)\s*/i, '');
		var html = '<div class="log-segment-wrapper">' +
			'<div class="log-segment-header" id="hdr-seg-' + id + '">' +
				'<table class="seg-header-table"><tr>' +
					'<td class="seg-td-icon"><span class="fold-icon">' + icon + '</span></td>' +
					'<td class="seg-td-lines"><span class="seg-lines">0</span> lines</td>' +
					'<td class="seg-td-errors"><span class="seg-errors"></span></td>' +
					'<td class="seg-td-title"><span class="seg-title">' + hsanitize(clean_title) + '</span></td>' +
				'</tr></table>' +
			'</div>' +
			'<div class="log-segment-body' + hideClass + '" id="seg-' + id + '">' +
			'</div>' +
		'</div>';
		
		// If root, we only append once, but wait, root is just flat.
		parent_dom.insertAdjacentHTML('beforeend', html);
		
		var seg_dom = document.getElementById("seg-" + id);
		append_chunk_table(id, 0, seg_dom);
		
		if (parent_id === "root") {
			if (document.getElementById("dlogsn")) document.getElementById("dlogsn").insertAdjacentHTML('beforeend', '<br><br>');
			if (document.getElementById("dlogst")) document.getElementById("dlogst").insertAdjacentHTML('beforeend', '<br><br>');
		}
	}
	segment_stack.push(seg);
}

function pop_segment() {
	if (segment_stack.length > 0) {
		flush_segments();

		var p = segment_stack[segment_stack.length - 1];
		if (p.auto_unfolded && p.error_count === 0 && !p.user_toggled) {
			var body = document.getElementById("seg-" + p.id);
			var hdr = document.getElementById("hdr-seg-" + p.id);
			if (body && !body.classList.contains("hide")) {
				body.classList.add("hide");
				if (hdr) {
					var icon = hdr.querySelector('.fold-icon');
					if (icon) icon.innerText = "▶";
				}
			}
		}

		segment_stack.pop();
		
		// When we return to parent, we need a new table below the children we just popped
		if (segment_stack.length > 0) {
			var p = segment_stack[segment_stack.length - 1];
			p.chunk_index++;
			var seg_dom = document.getElementById("seg-" + p.id);
			append_chunk_table(p.id, p.chunk_index, seg_dom);
		}
	}
}

function toggleSegment(id) {
	for (var i = 0; i < segment_stack.length; i++) {
		if (segment_stack[i].id == id) {
			segment_stack[i].user_toggled = true;
			break;
		}
	}
	var body = document.getElementById("seg-" + id);
	var hdr = document.getElementById("hdr-seg-" + id);
	if (body && hdr) {
		var icon = hdr.querySelector('.fold-icon');
		if (body.classList.contains("hide")) {
			body.classList.remove("hide");
			if (icon) icon.innerText = "▼";
		} else {
			body.classList.add("hide");
			if (icon) icon.innerText = "▶";
		}
	}
}

function ansiToHtml(text, state) {
    const classMap = {
        '1': 'ansi-bold', '4': 'ansi-underline',
        '30': 'ansi-fg-black', '31': 'ansi-fg-red', '32': 'ansi-fg-green', '33': 'ansi-fg-yellow', '34': 'ansi-fg-blue', '35': 'ansi-fg-magenta', '36': 'ansi-fg-cyan', '37': 'ansi-fg-white',
        '40': 'ansi-bg-black', '41': 'ansi-bg-red', '42': 'ansi-bg-green', '43': 'ansi-bg-yellow', '44': 'ansi-bg-blue', '45': 'ansi-bg-magenta', '46': 'ansi-bg-cyan', '47': 'ansi-bg-white',
    };

    // Ensure state is a valid object
    state = state || {};
    let currentClasses = new Set(state.classes || []);
    let currentLink = state.linkHref || null;

    const parts = text.split(/(\u001b\[[0-9:;<=>?]*[ -/]*[@-~]|\u001b\]8;.*?(?:\u001b\\|\x07))/);
    let html = '';

    for (const part of parts) {
        if (!part) continue;

        if (part.startsWith('\u001b[')) { // It's an ANSI code
            if (part.endsWith('m')) {
                const codes = part.substring(2, part.length - 1).split(';');

                for (let code of codes) {
                    if (code === '0' || code === '') {
                        currentClasses.clear();
                        continue;
                    }

                    /* handle leading zeros like 01 */
                    if (!classMap[code] && code.startsWith('0'))
                        code = code.substring(1);

                    if (classMap[code]) {
                        // Handle foreground/background colors: remove old before adding new
                        const icode = parseInt(code, 10);
                        if (icode >= 30 && icode <= 37) {
                            currentClasses.forEach(c => { if (c.startsWith('ansi-fg-')) currentClasses.delete(c); });
                        }
                        if (icode >= 40 && icode <= 47) {
                            currentClasses.forEach(c => { if (c.startsWith('ansi-bg-')) currentClasses.delete(c); });
                        }
                        currentClasses.add(classMap[code]);
                    }
                }
            }
            // Non-m sequences are just stripped (handled by the split and ignored here)
        } else if (part.startsWith('\u001b]8;')) { // OSC 8 Hyperlink
            let terminatorLen = part.endsWith('\x07') ? 1 : 2;
            let inner = part.substring(4, part.length - terminatorLen);
            let firstSemicolon = inner.indexOf(';');
            if (firstSemicolon !== -1) {
                let url = inner.substring(firstSemicolon + 1);
                if (url === "") {
                    currentLink = null;
                } else if (/^(https?|ftp|file|vscode):/i.test(url)) {
                    currentLink = url;
                }
            }
        } else { // It's plain text
            const sanitizedPart = hsanitize(part);
            let styledPart = sanitizedPart;
            if (currentClasses.size > 0) {
                styledPart = `<span class="${Array.from(currentClasses).join(' ')}">${styledPart}</span>`;
            }
            if (currentLink) {
                let safelink = hsanitize(currentLink);
                styledPart = `<a href="${safelink}" target="_blank" rel="noopener noreferrer">${styledPart}</a>`;
            }
            html += styledPart;
        }
    }

    return {
        html: html,
        newState: { classes: Array.from(currentClasses), linkHref: currentLink }
    };
}

function hsanitize(s)
{
	var table = {
		'<': 'lt',
		'>': 'gt',
		'"': 'quot',
		'\'': 'apos',
		'&': 'amp'
	};

	return s.toString().replace(/[<>"'&]/g, function(chr) {
		return '&' + table[chr] + ';';
	}).replace(/\r\n/g, '\n').replace(/\n/g, '<br>');
}

function createTaskRow(task, now_ut) {
    const tr = document.createElement("tr");
    tr.id = "task-row-" + task.task_uuid;

    let s1 = "";
    let qc;
    for (qc = 0; qc <= task.build_step; qc++)
        s1 += "&#9635;";
    while (qc <= task.total_steps) {
        s1 += "&#9633;";
        qc++;
    }

    let prefix = "";
    if (task.git_hash && task.repo_name) {
        prefix = `<span class="e6">${hsanitize(task.git_hash.substring(0, 4))}</span> ${hsanitize(task.repo_name)} `;
    }

    tr.innerHTML = `<td>${s1}</td>` +
                   `<td>${agify(now_ut, task.started)} ago</td>` +
                   `<td>${prefix}<a href="index.html?task=${hsanitize(task.task_uuid)}">${hsanitize(task.task_name)}</a></td>`;
    return tr;
}

function updateTaskRow(tr, task, now_ut) {
    let s1 = "";
    let qc;
    for (qc = 0; qc <= task.build_step; qc++)
        s1 += "&#9635;";
    while (qc <= task.total_steps) {
        s1 += "&#9633;";
        qc++;
    }
    let prefix = "";
    if (task.git_hash && task.repo_name) {
        prefix = `<span class="e6">${hsanitize(task.git_hash.substring(0, 4))}</span> ${hsanitize(task.repo_name)} `;
    }

    const newHTML = `<td>${s1}</td>` +
                   `<td>${agify(now_ut, task.started)} ago</td>` +
                   `<td>${prefix}<a href="index.html?task=${hsanitize(task.task_uuid)}">${hsanitize(task.task_name)}</a></td>`;
    
    if (tr.innerHTML !== newHTML) {
        tr.innerHTML = newHTML;
    }
}

function updateSpreadsheetDOM(container, tasks) {
	if (!tasks || !tasks.length) {
		container.innerHTML = "";
		return;
	}

    tasks.sort((a, b) => b.started - a.started || a.task_name.localeCompare(b.task_name));

    let table = container.querySelector("table.spreadsheet");
    if (!table) {
        container.innerHTML = '<table class="spreadsheet">' +
            '<thead><tr><th>Build Step</th><th>Since</th><th>Task</th></tr></thead>' +
            '<tbody></tbody></table>';
        table = container.querySelector("table.spreadsheet");
    }
    const tbody = table.querySelector("tbody");
    const now_ut = Math.round((new Date().getTime() / 1000));

    const existingRows = new Map();
    for (const row of tbody.children) {
        existingRows.set(row.id, row);
    }

    const newOrUpdatedTaskIds = new Set();
    for (const task of tasks) {
        const taskRowId = "task-row-" + task.task_uuid;
        newOrUpdatedTaskIds.add(taskRowId);
        const row = existingRows.get(taskRowId);

        if (row) {
            if (fadingTasks.has(task.task_uuid)) {
                clearTimeout(fadingTasks.get(task.task_uuid));
                fadingTasks.delete(task.task_uuid);
                row.classList.remove("fading-out");
            }
            updateTaskRow(row, task, now_ut);
        } else {
            tbody.appendChild(createTaskRow(task, now_ut));
        }
    }

    for (const [rowId, row] of existingRows) {
        if (!newOrUpdatedTaskIds.has(rowId)) {
            const task_uuid = rowId.substring(9);
            if (!fadingTasks.has(task_uuid)) {
                row.classList.add("fading-out");
                const timer = setTimeout(() => {
                    tbody.removeChild(row);
                    fadingTasks.delete(task_uuid);
                }, 3000);
                fadingTasks.set(task_uuid, timer);
            }
        }
    }

    const rows = Array.from(tbody.children);
    const taskMap = new Map(tasks.map(t => ["task-row-" + t.task_uuid, t]));

    rows.sort((rowA, rowB) => {
        const taskA = taskMap.get(rowA.id);
        const taskB = taskMap.get(rowB.id);
        if (!taskA || !taskB) return 0;
        return (taskB.started - taskA.started) || taskA.task_name.localeCompare(taskB.task_name);
    });

    for (let i = 0; i < rows.length; i++) {
        const expectedRow = rows[i];
        if (tbody.children[i] !== expectedRow) {
            tbody.insertBefore(expectedRow, tbody.children[i] || null);
        }
    }
}

var pos = 0, lli = 1, lines = "", times = "", locked = 1, tfirst = 0,
		cont = [ 0, 0, 0, 0, 0];
var deleted_events_cache = new Set();
var loaded_events = [], selected_event_uuid = null, selected_task_uuid = null, total_events = 0, current_offset = 0;

function sai_event_hash_display(hash) {
	if (!hash) return "";
	return "sai-" + hash.substring(0, 8);
}

function get_appropriate_ws_url()
{
	var pcol;
	var u = document.URL;

	/*
	 * We open the websocket encrypted if this page came on an
	 * https:// url itself, otherwise unencrypted
	 */

	if (u.substring(0, 5) === "https") {
		pcol = "wss://";
		u = u.substr(8);
	} else {
		pcol = "ws://";
		if (u.substring(0, 4) === "http")
			u = u.substr(7);
	}

	u = u.split("/");

	return pcol + u[0];
}

var age_names = [  "s",  "m",    "h", " days", " weeks", " months", " years" ];
var age_div =   [   1,   60,   3600,   86400,   604800,   2419200,  31536000  ];
var age_limit = [ 120, 7200, 172800, 1209600,  4838400,  63072000,         0  ];
var age_upd   = [   5,   10,    300,    1800,     3600, 12 * 3600, 12 * 3600  ];

function agify(now, secs)
{
	var d = now - secs, n;

	if (!secs)
		return "";

	if (secs > now)
		d = secs - now;

	for (n = 0; n < age_names.length; n++)
		if (d < age_limit[n] || age_limit[n] === 0)
			return "<span class='age-" + n + "' ut='" + secs +
				"'>" + ((secs > now) ? "in " : "") + Math.ceil(d / age_div[n]) +
				i18n(age_names[n]) + "</span>";
}

var aging_timer = null;

function aging()
{
	var n, next = 24 * 3600,
	    now_ut = Math.round((new Date().getTime() / 1000));

	var selector = [];
	for (n = 0; n < age_names.length; n++)
		selector.push(".age-" + n);

	var elems = document.querySelectorAll(selector.join(", "));
	var list = [];
	for (n = 0; n < elems.length; n++)
		list.push(elems[n]);

	for (n = 0; n < list.length; n++) {
		var e = list[n];
		var secs = e.getAttribute("ut");
		var d = Math.abs(now_ut - secs);

		for (var j = 0; j < age_limit.length; j++) {
			if (d < age_limit[j] || age_limit[j] === 0) {
				if (age_upd[j] < next)
					next = age_upd[j];
				break;
			}
		}

		e.outerHTML = agify(now_ut, secs);
	}

	if (next < 5)
		next = 5;

	/*
	 * We only need to come back when the age might have changed.
	 * Eg, if everything is counted in hours already, once per
	 * 5 minutes is accurate enough.
	 */
	if (aging_timer)
		clearTimeout(aging_timer);
	aging_timer = window.setTimeout(aging, next * 1000);
}
var sai, jso, s, sai_arts = "";

function sai_plat_icon(plat, size)
{
	var s, s1 = "";

	s = plat.split('/');
	if (s[0]) {
	// console.log("plat " + plat + " plat[0] " + s[0]);
	s1 = "<img class=\"ip" + size + " zup\" src=\"/sai/" + san(s[0]) +
		".svg\">";

	if (s[1])
		s1 += "<img class=\"ip" + size + " tread1\" src=\"/sai/arch-" + san(s[1]) + ".svg\">";
	}

	if (s[2]) {
		s1 += "<img class=\"ip" + size + " tread2\" src=\"/sai/tc-" + san(s[2]) + ".svg\">";
	}
	return s1;
}

function sai_stateful_taskname(state, nm, sf)
{
	var tp = "";

	if (sf)
		return "<span id=\"taskstate\" class=\"ti2 taskstate" +
			state + "\">&nbsp;" + san(nm) + "&nbsp;&nbsp;</span>";

	if (state == 4 || state == 6)
		tp = " ov_bad";

	return "<span id=\"taskstate\" class=\"ti2 " + tp + "\">" + san(nm) + "</span>";
}

function sai_taskinfo_render(t, now_ut)
{
	var now_ut = Math.round((new Date().getTime() / 1000));
	var s = "";

	s = "<table><tr class=\"nomar\"><td class=\"atop\"><table>" +
		sai_event_render(t, now_ut, 0) + "</table></td><td class=\"ti\">" +
		"<span class=\"ti1\">" + sai_plat_icon(t.t.platform, 2) +
		san(t.t.builder_name ? t.t.builder_name : t.t.platform) + "</span>&nbsp;" +
		sai_stateful_taskname(t.t.state, t.t.taskname, 1) + "&nbsp;&nbsp;";
	if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN && t.t.state != 0 && t.t.state != 3 && t.t.state != 4 && t.t.state != 5)
		s += "<img class=\"rebuild\" alt=\"stop build\" src=\"stop.svg\" " +
			"id=\"stop-" + san(t.t.uuid) + "\">&nbsp;";
	if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN)
		s += "<img class=\"rebuild\" alt=\"rebuild\" src=\"rebuild.png\" " +
			"id=\"rebuild-" + san(t.t.uuid) + "\">&nbsp;";

	if (t.t.builder_name) {
		var now_ut = Math.round((new Date().getTime() / 1000));

		if (t.t.started)
		/* started is a unix time, in seconds */
		s += "<span class=\"ti5\"> " +
		     agify(now_ut, t.t.started) + " ago, Dur: " +
		     (t.t.duration ? t.t.duration / 1000000 :
			now_ut - t.t.started).toFixed(1) +
			"s</span><div id=\"sai_arts\"></div><div id=\"metrics-summary-" + san(t.t.uuid) + "\"></div>";
		sai_arts = "";
	}

	if (t.runs && t.runs.length >= 2) {
		s += "<div class=\"runs-header-container\" style=\"display:flex; flex-wrap:wrap; gap:6px; margin-top:2px;\">";
		for (var n = t.runs.length - 1; n >= 0; n--) {
			var r = t.runs[n];
			var ridx = typeof r.run !== 'undefined' ? r.run : 0;
			var current = (ridx == (typeof t.t.run !== 'undefined' ? t.t.run : 0));
			var dcl = current ? "run-current-decal" : "run-decal";
			var timeStr = r.started ? agify(now_ut, r.started) + " ago" : "pending";
			var decal = "<div class=\"taskstate taskstate" + r.state + " " + dcl + "\" style=\"padding:4px; text-align:center; border-radius:6px;\">" +
				"<a href=\"index.html?task=" + t.t.uuid + "&run=" + ridx + "\" style=\"text-decoration:none; color:inherit; display:block;\">" +
				"<div>" + sai_plat_icon(r.platform, 0) + "</div>" +
				"<div class=\"ti5\" style=\"margin-top:2px;\">" + timeStr + "</div>" +
				"</a></div>";
			s += decal;
		}
		s += "</div>";
	}

	s += "</td></tr>";

	s += "</td></tr></table></table>";

	return s;
}

function update_summary_and_progress(event_uuid) {
    var sumbs = document.getElementById("sumbs-" + event_uuid);
    if (!sumbs)
        return;

    var summary = summarize_build_situation(event_uuid);
    var summary_html = summary.text;

    if (summary.total > 0 && summary.good !== summary.total) {
        var good_pct = (summary.good / summary.total) * 100;
        var pending_pct = (summary.pending / summary.total) * 100;
        var ongoing_pct = (summary.ongoing / summary.total) * 100;
        var bad_pct = (summary.bad / summary.total) * 100;

        var roundUpTo5 = function(n) {
            return Math.ceil(n / 5) * 5;
        };

        var good_w = roundUpTo5(good_pct);
        var pending_w = roundUpTo5(pending_pct);
        var ongoing_w = roundUpTo5(ongoing_pct);
        var bad_w = roundUpTo5(bad_pct);

        var total_w = good_w + pending_w + ongoing_w + bad_w;

        if (total_w > 100) {
            var surplus = total_w - 100;
            var widths = {good: good_w, pending: pending_w, ongoing: ongoing_w, bad: bad_w};

            var largest_key = Object.keys(widths).reduce(function(a, b){ return widths[a] > widths[b] ? a : b });

            widths[largest_key] -= surplus;

            good_w = widths.good;
            pending_w = widths.pending;
            ongoing_w = widths.ongoing;
            bad_w = widths.bad;
        }

        var good_cls = "w-" + good_w;
        var pending_cls = "w-" + pending_w;
        var ongoing_cls = "w-" + ongoing_w;
        var bad_cls = "w-" + bad_w;

        summary_html += "<div class=\"progress-bar\">" +
            "<div class=\"progress-bar-success " + good_cls + "\"></div>" +
            "<div class=\"progress-bar-pending " + pending_cls + "\"></div>" +
            "<div class=\"progress-bar-ongoing " + ongoing_cls + "\"></div>" +
            "<div class=\"progress-bar-failed float-right " + bad_cls + "\"></div>" +
            "</div>";
    }
    sumbs.innerHTML = summary_html;
}

function summarize_build_situation(event_uuid)
{
	var good = 0, bad = 0, total = 0, ongoing = 0, pending = 0;
	var ev_obj = null;
	if (typeof loaded_events !== 'undefined' && loaded_events) {
		ev_obj = loaded_events.find(o => o.e.uuid === event_uuid);
	}

	if (ev_obj && ev_obj.t) {
		var run_max = {};
		for (var q = 0; q < ev_obj.t.length; q++) {
			var tx = ev_obj.t[q];
			var ru = typeof tx.run !== 'undefined' ? tx.run : 0;
			if (typeof run_max[tx.uuid] === 'undefined' || ru > (typeof run_max[tx.uuid].run !== 'undefined' ? run_max[tx.uuid].run : 0))
				run_max[tx.uuid] = tx;
		}

		for (var uid in run_max) {
			var t = run_max[uid];
			total++;
			switch (t.state) {
				case 0: pending++; break;
				case 1:
				case 2:
				case 6: ongoing++; break;
				case 3: good++; break;
				case 4:
				case 5: bad++; break;
			}
		}
	} else {
		var roo = document.getElementById("taskcont-" + event_uuid);
		if (!roo)
			return { text: "" };

		var same = roo.querySelectorAll(".taskstate:not(.run-decal)");
		if (same)
			total = same.length;
		same = roo.querySelectorAll(".taskstate0:not(.run-decal)");
		if (same)
			pending = same.length;
		same = roo.querySelectorAll(".taskstate1:not(.run-decal)");
		if (same)
			ongoing += same.length;
		same = roo.querySelectorAll(".taskstate2:not(.run-decal)");
		if (same)
			ongoing += same.length;
		same = roo.querySelectorAll(".taskstate3:not(.run-decal)");
		if (same)
			good = same.length;
		same = roo.querySelectorAll(".taskstate4:not(.run-decal)");
		if (same)
			bad += same.length;
		same = roo.querySelectorAll(".taskstate5:not(.run-decal)");
		if (same)
			bad += same.length;
		same = roo.querySelectorAll(".taskstate6:not(.run-decal)");
		if (same)
			ongoing += same.length;
	}

	var text;
	if (good == total && total > 0)
		text = "All " + good + " passed";
	else if (bad == total && total > 0)
		text = "All " + bad + " failed";
	else if (pending == total && total > 0)
		text = total + " pending";
	else {
		var parts = [];
		if (good) parts.push("OK: " + good);
		if (bad) parts.push("Bad: " + bad);
		if (ongoing) parts.push("Building: " + ongoing);
		if (pending) parts.push("Wait: " + pending);
		text = parts.join(", ");
	}

	return {
		text: text,
		good: good,
		bad: bad,
		ongoing: ongoing,
		pending: pending,
		total: total
	};
}

function sai_watcher_render(w) {
	var s = "", svc = null;

	/* Find service definition */
	if (watcher_services && watcher_services.watchers) {
		watcher_services.watchers.forEach(sv => {
			if (sv.name === w.service_name) svc = sv;
		});
	}

	s = "<div class=\"watcher\" title=\"" + san(w.service_name) + "\">";
	s += "<a href=\"" + san(w.url) + "\" target=\"_blank\">";
	s += "<img src=\"/sai/watchers/" + san(w.service_name) + "/icon.svg\" class=\"watcher-icon\">";
	s += "</a>";

	if (w.metrics_json) {
		try {
			var m = JSON.parse(w.metrics_json);
			if (svc && svc.ui) {
				s += "<div class=\"watcher-metrics\">";
				svc.ui.forEach(u => {
					if (typeof m[u.key] !== 'undefined') {
						var val = m[u.key];
						var cl = "";
						if (typeof u.fail_if_gt !== 'undefined' && parseInt(val) > u.fail_if_gt) cl = " watcher-fail";
						else if (typeof u.warn_if_gt !== 'undefined' && parseInt(val) > u.warn_if_gt) cl = " watcher-warn";

						s += "<span class=\"watcher-metric" + cl + "\" title=\"" + san(u.label) + "\">" + san(val) + "</span>";
					}
				});
				s += "</div>";
			}
		} catch (e) { }
	}
	s += "</div>";

	return s;
}

function sai_event_summary_render(o, now_ut, reset_all_icon)
{
	var s, q, ctn = "", wai, s1 = "", n, e = o.e;

	s = "<table class=\"comp";

	if (!o.e)
		return;

	if (e.state == 3)
		s += " comp_pass";
	if (e.state == 4 || e.state == 6)
		s += " comp_fail";

	s += "\"><tr><td class=\"jumble\"><a href=\"/sai/?event=" + san(e.uuid) +
		"\"><img src=\"/sai/sai-event.svg\"";
	if (gitohashi_integ)
		s += " class=\"saicon\"";
	if (e.state == 3 || e.state == 4)
		s += " class=\"deemph\"";
	s += ">";
	var cl = "evr";
	if (gitohashi_integ)
		cl = "evr_gi";
	if (e.state == 3)
		s += "<div class=\"" + cl + "\"><img src=\"/sai/passed.svg\"></div>";
	if (e.state == 4)
		s += "<div class=\"" + cl + "\"><img src=\"/sai/failed.svg\"></div>";

	s += "</a>";
	if (reset_all_icon && !gitohashi_integ && auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
		s += "<br><img class=\"rebuild\" alt=\"rebuild all\" src=\"/sai/rebuild.png\" " +
			"id=\"rebuild-ev-" + san(e.uuid) + "\">&nbsp;";
		s += "<img class=\"rebuild\" alt=\"delete event\" src=\"/sai/delete.png\" " +
				"id=\"delete-ev-" + san(e.uuid) + "\">";
	}
	s += "</td>";

	if (!gitohashi_integ) {
		s +=
		"<td><table class=\"nomar\">" +
		"<tr><td class=\"nomar\" colspan=2>" +
		"<span class=\"e1\">" + san(e.repo_name);
		if (e.sec)
			s += " <img class=\"bico\" src=\"/sai/locked.svg\">";
		s += "</span></td></tr><tr><td class=\"nomar\" colspan=2><span class=\"e2\">";

		if (e.ref.substr(0, 11) === "refs/heads/") {
			s += "<img class=\"branch\">" +
				san(e.ref.substr(11));
		} else
			if (e.ref.substr(0, 10) === "refs/tags/") {
				s += "<img class=\"tag\">" +
					san(e.ref.substr(10));
			} else
				s += san(e.ref);

		s += "</span></td></tr><tr><td class=\"nomar e6\">" +
		        san(sai_event_hash_display(e.hash)) +
		     "</td><td class=\"e6 nomar\">" +
		     agify(now_ut, e.created) + "</td></tr>";
		 s += "</table>" +
		     "</td>";
	} else {
		s +="<td><table><tr><td class=\"e6 nomar\">" + san(sai_event_hash_display(e.hash)) + " " + agify(now_ut, e.created) +
		     "</td></tr><tr><td class=\"nomar e6\" id=\"sumbs-" + e.uuid + "\"></td></tr>" +
		     "</table></td>";
	}
	s += "</tr>";

	if (o.watchers && o.watchers.length) {
		s += "<tr><td class=\"nomar\" colspan=\"2\"><div class=\"watchers-row\">";
		o.watchers.forEach(w => {
			s += sai_watcher_render(w);
		});
		s += "</div></td></tr>";
	}

	s += "<tr><td class=\"nomar e6\" colspan=\"2\" id=\"sumbs-" + e.uuid +"\"></td></tr></table>";

	return s;
}

function find_event_by_task_uuid(taskUuid) {
	if (!loaded_events) return null;
	for (var i = 0; i < loaded_events.length; i++) {
		var o = loaded_events[i];
		if (o.t) {
			for (var j = 0; j < o.t.length; j++) {
				if (o.t[j].uuid === taskUuid) {
					return o.e.uuid;
				}
			}
		}
	}
	return null;
}

function render_event_decals() {
	var now_ut = Math.round((new Date().getTime() / 1000));
	var s = "";
	
	// Add "Newer" pagination button if applicable
	if (!gitohashi_integ && total_events > 6 && current_offset > 0) {
		s += "<div class=\"btn sai-pagination-btn pagination-card\" data-offset=\"" + Math.max(0, current_offset - 6) + "\">&lt; Newer</div>";
	}

	if (loaded_events && loaded_events.length) {
		for (var n = loaded_events.length - 1; n >= 0; n--) {
			var o = loaded_events[n];
			var isSelected = (o.e.uuid === selected_event_uuid);
			var stateClass = "";
			if (o.e && o.e.state == 3) stateClass = "comp_pass";
			if (o.e && (o.e.state == 4 || o.e.state == 6)) stateClass = "comp_fail";
			s += "<div class=\"event-decal-card " + stateClass + (isSelected ? " selected" : "") + "\" data-uuid=\"" + san(o.e.uuid) + "\">";
			s += sai_event_summary_render(o, now_ut, 1);
			s += "</div>";
		}
	} else {
		s += "<div class=\"no-events\">No events found</div>";
	}

	// Add "Older" pagination button if applicable
	if (!gitohashi_integ && total_events > 6 && current_offset + 6 < total_events) {
		s += "<div class=\"btn sai-pagination-btn pagination-card\" data-offset=\"" + (current_offset + 6) + "\">Older &gt;</div>";
	}

	var container = document.getElementById("sai_event_decals");
	if (container) {
		container.innerHTML = s;
		
		container.querySelectorAll(".event-decal-card").forEach(function(card) {
			card.addEventListener("click", function() {
				var uuid = card.getAttribute("data-uuid");
				selectEvent(uuid);
			});
		});

		// Scroll the selected card into view
		var selectedCard = container.querySelector(".event-decal-card.selected");
		if (selectedCard) {
			selectedCard.scrollIntoView({ behavior: "smooth", block: "nearest", inline: "nearest" });
		}
	}

	// Refresh summaries/progress bars
	if (loaded_events) {
		loaded_events.forEach(o => {
			update_summary_and_progress(o.e.uuid);
		});
	}
}

function render_selected_event_tasks(o) {
	var now_ut = Math.round((new Date().getTime() / 1000));
	var s = "";
	var e = o.e;
	if (o.t && o.t.length) {
		s += "<div class=\"event-tasks-header\">";
		var refName = e.ref.replace("refs/heads/", "").replace("refs/tags/", "");
		s += "<span class=\"event-tasks-title\">" + san(e.repo_name) + " (" + san(refName) + ") - " + san(sai_event_hash_display(e.hash)) + "</span>";
		s += "</div>";
		s += "<table class=\"tasks-table-display\"><tr><td class=\"tasks\" id=\"taskcont-" + san(e.uuid) + "\">";

		var run_max = {}, run_list = {};
		for (var q = 0; q < o.t.length; q++) {
			var tx = o.t[q];
			var ru = typeof tx.run !== 'undefined' ? tx.run : 0;
			if (!run_list[tx.uuid]) run_list[tx.uuid] = [];
			run_list[tx.uuid].push(tx);
			if (typeof run_max[tx.uuid] === 'undefined' || ru > (typeof run_max[tx.uuid].run !== 'undefined' ? run_max[tx.uuid].run : 0))
				run_max[tx.uuid] = tx;
		}
		for (var uid in run_list) {
			run_list[uid].sort(function(a, b) { var ar = typeof a.run !== 'undefined' ? a.run : 0; var br = typeof b.run !== 'undefined' ? b.run : 0; return ar - br; });
		}

		var ctn = "";
		var s1 = "";
		for (var q = 0; q < o.t.length; q++) {
			var t = o.t[q];

			if (t !== run_max[t.uuid])
				continue;

			if (t.taskname !== ctn) {
				if (ctn !== "") {
					s += "<div class=\"ib\"><table class=\"nomar\">" +
					     "<tr><td class=\"tn\">" + hsanitize(ctn) +
					     "</td><td class=\"keepline\">" + s1 +
					     "</td></tr></table></div>";
					s1 = "";
				}
				ctn = t.taskname;
			}

			s1 += "<div id=\"taskstate_" + t.uuid + "\" class=\"taskstate taskstate" + t.state +
				(run_list[t.uuid].length > 1 ? " has_runs" : "") +
				"\" data-event-uuid=\"" + san(e.uuid) + "\" data-platform=\"" + san(t.platform) +
				"\" data-rebuildable=\"" + t.rebuildable + "\">";
			s1 += "<a href=\"index.html?task=" + t.uuid + "\">" +
				sai_plat_icon(t.platform, 0) + "</a>";
			if (run_list[t.uuid].length > 1) {
				s1 += "<div class=\"runs-popup\"><table>";
				for (var w = 0; w < run_list[t.uuid].length; w++) {
					var rt = run_list[t.uuid][w];
					var rr = typeof rt.run !== 'undefined' ? rt.run : 0;
					var decal = "<div class=\"taskstate taskstate" + rt.state + " run-decal\"><a href=\"index.html?task=" + t.uuid + "&run=" + rr + "\">" + sai_plat_icon(rt.platform, 0) + "</a></div>";
					var timeStr = rt.started ? agify(now_ut, rt.started) + " ago" : "pending";
					s1 += "<tr><td>" + decal + "</td><td class=\"runs-time-cell\"><span class=\"ti5\">" + timeStr + "</span></td></tr>";
				}
				s1 += "</table></div>";
			}
			s1 += "</div>";
		}

		if (ctn !== "") {
			s += "<div class=\"ib\"><table class=\"nomar\">" +
				"<tr><td class=\"tn\">" + hsanitize(ctn) +
				"<td class=\"keepline\">" + s1 +
				"</td></tr></table></div>";
		}

		s += "</td></tr></table>";
	} else {
		s += "<div class=\"no-tasks\">No tasks for this event</div>";
	}

	var container = document.getElementById("sai_event_tasks");
	if (container) {
		container.innerHTML = s;
		
		// Refresh progress bars for these tasks
		if (o.t) {
			for (var q = 0; q < o.t.length; q++) {
				refresh_state(o.t[q]);
			}
		}
		update_summary_and_progress(e.uuid);
	}
}

function selectEvent(uuid) {
	selected_event_uuid = uuid;

	// Check if selected task belongs to this event
	var ev_obj = loaded_events.find(o => o.e.uuid === uuid);
	var hasTask = false;
	if (ev_obj && ev_obj.t && selected_task_uuid) {
		hasTask = ev_obj.t.some(t => t.uuid === selected_task_uuid);
	}
	if (!hasTask) {
		selected_task_uuid = null;
		window.current_task_run = null;
		var stickyEl = document.getElementById("sai_sticky");
		var overviewEl = document.getElementById("sai_overview");
		if (stickyEl) stickyEl.innerHTML = "";
		if (overviewEl) overviewEl.innerHTML = "";
		
		lines = times = logs = "";
		lines_pending = times_pending = logs_pending = "";
		segment_stack = [];
		seg_counter = 0;
		window.held_start_line = null;
		logAnsiState = {};
		tfirst = 0;
		lli = 1;
		last_log_timestamp = 0;
	}

	var par = new URLSearchParams(window.location.search);
	par.set("event", uuid);
	if (!hasTask) {
		par.delete("task");
		par.delete("run");
	}
	var qs = par.toString();
	var path = window.location.pathname;
	if (!path.endsWith('/') && !path.endsWith('index.html')) {
		path += '/';
	}
	window.history.pushState({}, "", path + (qs ? ("?" + qs) : ""));
	
	// Highlight card
	var container = document.getElementById("sai_event_decals");
	if (container) {
		container.querySelectorAll(".event-decal-card").forEach(function(card) {
			if (card.getAttribute("data-uuid") === uuid) {
				card.classList.add("selected");
			} else {
				card.classList.remove("selected");
			}
		});
	}

	if (ev_obj) {
		render_selected_event_tasks(ev_obj);
		/* Notify server of the selected event so it can throttle task state broadcasts */
		sai.send("{\"schema\":\"com.warmcat.sai.eventinfo\", \"js_api_version\": " + SAI_JS_API_VERSION + ", \"event_hash\": " + JSON.stringify(uuid) + "}");
	}
}

function init_task_logs_dom() {
	var s = "<table><td colspan=\"3\"><pre><table class=\"scrollogs\"><tr>" +
			"<td class=\"atop\">" +
			"<div id=\"dlogsn\" class=\"dlogsn\">" + lines + "</div></td>" +
			"<td class=\"atop\">" +
			"<div id=\"dlogst\" class=\"dlogst\">" + times + "</div></td>" +
			 "<td class=\"atop\"><div id=\"dlogs\" class=\"dlogs\">" +
			 "<span id=\"logs\" class=\"nowrap\">" + logs +
			 "</span>"+
			 "</div></td></tr></table></pre>";
	var overviewEl = document.getElementById("sai_overview");
	if (overviewEl) {
		overviewEl.innerHTML = s;
	}
}

function selectTask(taskUuid, runVal) {
	selected_task_uuid = taskUuid;
	window.current_task_run = runVal;

	// Update URL query parameters dynamically (fully relative)
	var par = new URLSearchParams(window.location.search);
	par.set("task", taskUuid);
	if (runVal && runVal !== "-1") {
		par.set("run", runVal);
	} else {
		par.delete("run");
	}
	var path = window.location.pathname;
	if (!path.endsWith('/') && !path.endsWith('index.html')) {
		path += '/';
	}
	window.history.pushState({}, "", path + "?" + par.toString());

	// Setup loading state and clear logs (without the loading text overlay)
	var stickyEl = document.getElementById("sai_sticky");
	if (stickyEl) {
		stickyEl.innerHTML = "<div class=\"taskinfo\" id=\"taskinfo-" + san(taskUuid) + "\"></div>";
	}

	lines = times = logs = "";
	lines_pending = times_pending = logs_pending = "";
	segment_stack = [];
	seg_counter = 0;
	window.held_start_line = null;
	logAnsiState = {};
	tfirst = 0;
	lli = 1;
	last_log_timestamp = 0;

	init_task_logs_dom();

	// Request logs from websocket
	var req = "{\"schema\":" +
		  "\"com.warmcat.sai.taskinfo\"," +
		  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
		  "\"logs\": 1," +
		  "\"last_log_ts\":" + last_log_timestamp + ",";
	if (runVal && runVal !== "-1")
		 req += "\"run\":" + runVal + ",";
	 else
		 req += "\"run\": -1,";
	req += "\"task_hash\":" + JSON.stringify(taskUuid) + "}";
	sai.send(req);
}

function sai_event_render(o, now_ut, reset_all_icon)
{
	var s, q, ctn = "", wai, s1 = "", n, e = o.e;

	s = "<tr><td class=\"waiting\"";
	if (gitohashi_integ)
		s += " id=\"gitohashi_sai_icon\"";
	s += "><div id=\"esr-" + san(e.uuid) + "\"></div></td>";

	if (o.t.length) {
		s += "<td class=\"tasks\" id=\"taskcont-" + san(e.uuid) + "\">";
		if (gitohashi_integ)
			s += "<div class=\"gi_popup\" id=\"gitohashi_sai_details\">";

		s += "<table><tr><td class=\"atop\">";

		var run_max = {}, run_list = {};
		for (q = 0; q < o.t.length; q++) {
			var tx = o.t[q];
			var ru = typeof tx.run !== 'undefined' ? tx.run : 0;
			if (!run_list[tx.uuid]) run_list[tx.uuid] = [];
			run_list[tx.uuid].push(tx);
			if (typeof run_max[tx.uuid] === 'undefined' || ru > (typeof run_max[tx.uuid].run !== 'undefined' ? run_max[tx.uuid].run : 0))
				run_max[tx.uuid] = tx;
		}
		for (var uid in run_list) {
			run_list[uid].sort(function(a, b) { var ar = typeof a.run !== 'undefined' ? a.run : 0; var br = typeof b.run !== 'undefined' ? b.run : 0; return ar - br; });
		}

		for (q = 0; q < o.t.length; q++) {
			var t = o.t[q];

			if (t !== run_max[t.uuid])
				continue;

			if (t.taskname !== ctn) {
				if (ctn !== "") {
					s += "<div class=\"ib\"><table class=\"nomar\">" +
					     "<tr><td class=\"tn\">" + ctn +
					     "</td><td class=\"keepline\">" + s1 +
					     "</td></tr></table></div>";
					s1 = "";
				}
				ctn = t.taskname;
			}

			s1 += "<div id=\"taskstate_" + t.uuid + "\" class=\"taskstate taskstate" + t.state +
				(run_list[t.uuid].length > 1 ? " has_runs" : "") +
				"\" data-event-uuid=\"" + san(e.uuid) + "\" data-platform=\"" + san(t.platform) +
				"\" data-rebuildable=\"" + t.rebuildable + "\">";
			s1 += "<a href=\"/sai/index.html?task=" + t.uuid + "\">" +
				sai_plat_icon(t.platform, 0) + "</a>";
			if (run_list[t.uuid].length > 1) {
				s1 += "<div class=\"runs-popup\"><table>";
				for (var w = 0; w < run_list[t.uuid].length; w++) {
					var rt = run_list[t.uuid][w];
					var rr = typeof rt.run !== 'undefined' ? rt.run : 0;
					var decal = "<div class=\"taskstate taskstate" + rt.state + " run-decal\"><a href=\"/sai/index.html?task=" + t.uuid + "&run=" + rr + "\">" + sai_plat_icon(rt.platform, 0) + "</a></div>";
					var timeStr = rt.started ? agify(now_ut, rt.started) + " ago" : "pending";
					s1 += "<tr><td>" + decal + "</td><td class=\"runs-time-cell\"><span class=\"ti5\">" + timeStr + "</span></td></tr>";
				}
				s1 += "</table></div>";
			}
			s1 += "</div>";
		}

		if (ctn !== "") {
			s += "<div class=\"ib\"><table class=\"nomar\">" +
				"<tr><td class=\"tn\">" + ctn +
				"<td class=\"keepline\">" + s1 +
				"</td></tr></table></div>";
		}

		s += "</td></tr></table>";
		if (gitohashi_integ)
			s += "</div>";
		s += "</td>";
	}

	s += "</tr>";

	return "<tbody id=\"ev-group-" + o.e.uuid + "\">" + s + "</tbody>";
}

function getBuilderHostname(platName) {
	return platName.split('.')[0];
}

function getBuilderGroupKey(platName) {
	let hostname = platName.split('.')[0];
	if (hostname.includes('-')) {
		let parts = hostname.split('-');
		return parts[parts.length - 1];
	}
	return hostname;
}

window.current_viewed_task_state = 0;

function check_and_apply_failure_ui() {
	if (window.current_viewed_task_state === 4 || window.current_viewed_task_state === 5 || window.current_viewed_task_state === 6) {
		var rootDlogs = document.getElementById("dlogs");
		if (rootDlogs) {
			var wrappers = rootDlogs.querySelectorAll(".log-segment-wrapper > .log-segment-header");
			/* Only want the top-level ones, which are direct children of #dlogs > .log-segment-wrapper */
			var topWrappers = [];
			for (var i = 0; i < wrappers.length; i++) {
				if (wrappers[i].parentElement && wrappers[i].parentElement.parentElement === rootDlogs) {
					topWrappers.push(wrappers[i]);
				}
			}
			if (topWrappers.length > 0) {
				var hdr = topWrappers[topWrappers.length - 1];
				hdr.classList.add("seg-fail-red");
				var body = hdr.nextElementSibling;
				if (body && body.classList.contains("hide")) {
					body.classList.remove("hide");
					var icon = hdr.querySelector('.fold-icon');
					if (icon) icon.innerText = "▼";
				}
			}
		}
	}
}

function refresh_state(t)
{
	var task_uuid = t.uuid;
	var task_state = t.state;
	var els = document.querySelectorAll("[id='taskstate_" + task_uuid + "']");

	els.forEach(function(tsi) {
		tsi.classList.remove("taskstate0");
		tsi.classList.remove("taskstate1");
		tsi.classList.remove("taskstate2");
		tsi.classList.remove("taskstate3");
		tsi.classList.remove("taskstate4");
		tsi.classList.remove("taskstate5");
		tsi.classList.remove("taskstate6");
		tsi.classList.remove("taskstate7");
		tsi.classList.remove("taskstate10");
		tsi.classList.add("taskstate" + task_state);

		var toRemove = [];
		for (var i = 0; i < tsi.classList.length; i++) {
			if (tsi.classList[i].startsWith('prog-')) {
				toRemove.push(tsi.classList[i]);
			}
		}
		toRemove.forEach(function(cls) { tsi.classList.remove(cls); });

		if (task_state === 1 || task_state === 2 || task_state === 6) {
			var total = typeof t.total_steps !== 'undefined' ? t.total_steps : t.build_step_count;
			if (typeof t.build_step !== 'undefined' && typeof total !== 'undefined' && total >= 0) {
				var pct = Math.round((t.build_step + 1) * 100 / (total + 2));
				if (pct > 100) pct = 100;
				if (pct < 0) pct = 0;
				pct = Math.round(pct / 5) * 5;
				tsi.classList.add("prog-" + pct);
			}
		}
	});

	const urlParams = new URLSearchParams(window.location.search);
	const urlTask = urlParams.get('task');
	if (urlTask && urlTask === task_uuid) {
		window.current_viewed_task_state = task_state;
		check_and_apply_failure_ui();
	}
}



function createContextMenu(event, menuItems) {
    event.preventDefault();

    // Remove any existing context menu
    const existingMenus = document.querySelectorAll(".context-menu");
    existingMenus.forEach(menu => {
        if (document.body.contains(menu))
            document.body.removeChild(menu);
    });

    const menu = document.createElement("div");
    menu.className = "context-menu";
    menu.style.top = event.pageY + "px";
    menu.style.left = event.pageX + "px";

    const ul = document.createElement("ul");
    menu.appendChild(ul);

    /*
     * We have to do this via a function because the event listener
     * for the global click needs to be removable, but the click
     * handler for the menu items also wants to use it.
     */
    const closeMenu = () => {
        if (document.body.contains(menu)) {
            document.body.removeChild(menu);
        }
        window.removeEventListener("click", closeMenu, true);
    };

    menuItems.forEach(item => {
        const li = document.createElement("li");
        li.innerHTML = item.label;
        if (item.callback) {
            li.addEventListener("click", (e) => {
                item.callback(e);
                closeMenu();
            });
        } else {
            li.classList.add("read-only");
        }
        ul.appendChild(li);
    });

    document.body.appendChild(menu);

    /*
     * Now we have the content, we can see how big it is.  If it
     * is going off the right of the page, move it left so it ends
     * at the click coordinates.
     */

    const rect = menu.getBoundingClientRect();
    if (rect.right > window.innerWidth)
         menu.style.left = (event.pageX - rect.width) + "px";

    /*
     * defer adding the click listener so the current click
     * doesn't trigger it.  Use capture on window so we get
     * it even if the click target stops propagation.
     */
    setTimeout(() => {
        window.addEventListener("click", closeMenu, true);
    }, 0);
}

function createBuilderDiv(plat) {
	const platDiv = document.createElement("div");
	platDiv.className = "ibuil bdr";
	if (plat.name.startsWith("sai-vm-")) {
		platDiv.className += " vm-builder";
	}
	if (!plat.online)
		platDiv.className += " offline";
	else {
		if (!plat.power_managed)
			platDiv.className += " power-unmanaged";
		else {
			let pcon = pcon_topology[plat.pcon];
			if (pcon && pcon.manual_on)
				platDiv.className += " power-stay";
			else
				platDiv.className += " power-stay-dep";
		}
	}
	if (plat.powering_up)
		platDiv.className += " powering-up";
	if (plat.powering_down)
		platDiv.className += " powering-down";

	platDiv.id = "binfo-" + plat.name;
	platDiv.title = plat.platform + "@" + plat.name.split('.')[0] + (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN && plat.peer_ip ? " / " + plat.peer_ip : "");

	let plat_parts = plat.platform.split('/');
	let plat_os = plat_parts[0] || 'generic';
	let plat_arch = plat_parts[1] || 'generic';
	let plat_tc = plat_parts[2] || 'generic';
	let short_name = plat.name.split('.')[0];

	let innerHTML = `<table class="nomar"><tbody><tr><td class="bn">`;
	innerHTML += `<div class="builder-name-row">` +
		     `<div class="builder-short-name">${hsanitize(short_name)}</div>` +
		     `<div class="builder-icons">` +
		     `<img class="ip1 zup" data-sai-src="/sai/${plat_os}.svg">` +
		     `<img class="ip1 tread1" data-sai-src="/sai/arch-${plat_arch}.svg">` +
		     `<img class="ip1 tread2" data-sai-src="/sai/tc-${plat_tc}.svg">` +
		     `</div></div>`;
	innerHTML += `<div class="resource-bars">` +
		     `<div class="res-bar"><div class="res-bar-inner res-bar-cpu w-0"></div></div>` +
		     `<div class="res-bar"><div class="res-bar-inner res-bar-ram w-0"></div></div>` +
		     `<div class="res-bar"><div class="res-bar-inner res-bar-disk w-0"></div></div>` +
		     `</div>`;
	if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN && plat.peer_ip)
		innerHTML += `<div class="plat-peer-ip">${hsanitize(plat.peer_ip)}</div>`;
	innerHTML +=  `</td></tr></tbody></table>`;

	platDiv.innerHTML = innerHTML;

	const images = platDiv.querySelectorAll('img[data-sai-src]');
	images.forEach(img => {
		img.onerror = () => {
			img.src = '/sai/generic.svg';
			img.onerror = null; // prevent infinite loops
		};
		img.src = img.getAttribute('data-sai-src');
	});

	const menuItems = [
		{ label: `<b>SAI:</b> ${san(plat.sai_hash)}` },
		{ label: `<b>LWS:</b> ${san(plat.lws_hash)}` },
	];

	if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN && !plat.online) {
		menuItems.push({
			label: "<span class='builder-delete-btn'>Delete Builder</span>",
			callback: () => {
				if (confirm("Are you sure you want to delete builder " + plat.name + "?")) {
					const msg = {
						schema: "com.warmcat.sai.builderdelete",
						builder_name: plat.name
					};
					sai.send(JSON.stringify(msg));
				}
			}
		});
	}

	if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
		menuItems.push({
			label: "<span class='builder-shell-btn'>Open Shell</span>",
			callback: () => {
				const task_uuid = Array.from(crypto.getRandomValues(new Uint8Array(8)))
					.map(b => b.toString(16).padStart(2, '0')).join('');

				const msg = {
					schema: "com.warmcat.sai.openshell",
					builder_name: plat.name,
					task_uuid: task_uuid
				};
				sai.send(JSON.stringify(msg));

				const term = new SaiTerminal(document.body, {
					title: "Terminal: " + plat.name,
					platform: plat.platform,
					onData: (data) => {
						const ptyMsg = {
							schema: "com.warmcat.sai.ptydata",
							builder_name: plat.name,
							task_uuid: task_uuid,
							channel: 0,
							data: btoa(data),
							len: data.length
						};
						sai.send(JSON.stringify(ptyMsg));
					},
					onResize: (cols, rows) => {
						const resizeMsg = {
							schema: "com.warmcat.sai.ptydata",
							builder_name: plat.name,
							task_uuid: task_uuid,
							channel: 0,
							cols: cols,
							rows: rows,
							data: "",
							len: 0
						};
						sai.send(JSON.stringify(resizeMsg));
					},
					onClose: () => {
						const closeMsg = {
							schema: "com.warmcat.sai.closeshell",
							task_uuid: task_uuid
						};
						sai.send(JSON.stringify(closeMsg));
						delete active_terminals[task_uuid];
					}
				});
				active_terminals[task_uuid] = term;
			}
		});
	}

	platDiv.addEventListener("contextmenu", function(event) {
		if (!authd)
			return;
		createContextMenu(event, menuItems);
	});

    let touchStartTime = 0;
    let touchStartPos = { x: 0, y: 0 };

    platDiv.addEventListener("touchstart", function(event) {
        if (event.touches.length > 1) {
            return;
        }
        touchStartTime = Date.now();
        const touch = event.touches[0];
        touchStartPos = { x: touch.pageX, y: touch.pageY };
    });

    platDiv.addEventListener("touchend", function(event) {
        const touchEndTime = Date.now();
        const touch = event.changedTouches[0];
        const touchEndPos = { x: touch.pageX, y: touch.pageY };
        const pressDuration = touchEndTime - touchStartTime;
        const distance = Math.sqrt(
            Math.pow(touchEndPos.x - touchStartPos.x, 2) +
            Math.pow(touchEndPos.y - touchStartPos.y, 2)
        );

        if (pressDuration >= 500 && distance < 10) {
            event.preventDefault();

            const mockEvent = {
                preventDefault: () => {},
                pageX: touchStartPos.x,
                pageY: touchStartPos.y
            };
            if (authd)
		createContextMenu(mockEvent, menuItems);
        }
        touchStartTime = 0;
    });

	return platDiv;
}

function updateSpreadsheetCell(cell, platName) {
	let best_match_key = null;
	for (const short_name in spreadsheet_data_cache) {
		if (platName.startsWith(short_name)) {
			if (!best_match_key || short_name.length > best_match_key.length) {
				best_match_key = short_name;
			}
		}
	}

	if (best_match_key) {
		updateSpreadsheetDOM(cell, spreadsheet_data_cache[best_match_key]);
		aging();
	} else {
		cell.innerHTML = ""; // Clear it if no data
	}
}

function createBuilderRow(plat) {
	const tr = document.createElement("tr");
	tr.id = "row-" + plat.name;

	const tdInfo = document.createElement("td");
	tdInfo.className = "builder-info";
	const builderDiv = createBuilderDiv(plat);
	tdInfo.appendChild(builderDiv);
	tr.appendChild(tdInfo);

	const tdSpreadsheet = document.createElement("td");
	tdSpreadsheet.className = "spreadsheet-container";
	tdSpreadsheet.id = "spreadsheet-" + plat.name;
	updateSpreadsheetCell(tdSpreadsheet, plat.name);
	tr.appendChild(tdSpreadsheet);

	return tr;
}

function updateBuilderRow(row, plat) {
	const tdInfo = row.querySelector(".builder-info");
	const tdSpreadsheet = row.querySelector(".spreadsheet-container");

	// Update builder info div
	// This is simple enough that a full replacement is fine and ensures listeners are correct.
	tdInfo.innerHTML = "";
	tdInfo.appendChild(createBuilderDiv(plat));

	// Update spreadsheet view for this builder
	updateSpreadsheetCell(tdSpreadsheet, plat.name);
}

/* Global caches for reconcilation */
var pcon_topology = {};
var last_builder_list = [];

function createPconDiv(pcon) {
    const pconDiv = document.createElement("div");
    pconDiv.className = "pcon";
    pconDiv.id = "pcon-" + pcon.name;
    pconDiv.style.marginLeft = "10px";
    pconDiv.style.borderLeft = "1px solid #ccc";
    pconDiv.style.paddingLeft = "5px";

    const header = document.createElement("div");
    header.className = "pcon-header";

    const myBuilders = last_builder_list.filter(b => b.pcon === pcon.name);
    let anyConnected = myBuilders.some(b => b.online === 1);
    let anyPoweringUp = myBuilders.some(b => b.powering_up === 1);

    let isActuallyOn = false;
    let stateClass = "pcon-off";

    if (pcon_energy_cache[pcon.name]) {
        const d = pcon_energy_cache[pcon.name];
        let hasPower = d.voltage_v >= 70 && d.active_power_w > 0;
        isActuallyOn = anyConnected || anyPoweringUp || hasPower;
        stateClass = hasPower ? "pcon-on" : "pcon-off";
    } else {
        isActuallyOn = anyConnected || (pcon.on === 1 && !anyPoweringUp);
        stateClass = (pcon.on === 1) ? "pcon-on" : "pcon-off";
    }

    if (isActuallyOn) {
        header.className += " pcon-header-on";
    }

    let type = pcon.type ? `(${pcon.type})` : "";

    header.innerHTML = `<span class="${stateClass}">&#x23FB;</span> <b>${hsanitize(pcon.name)}</b> <span class="pcon-type">${hsanitize(type)}</span>`;

    if (pcon_energy_cache[pcon.name]) {
        const d = pcon_energy_cache[pcon.name];
        let stats = document.createElement("span");
        stats.className = "pcon-stats";
        stats.style.marginLeft = "10px";
        stats.style.fontSize = "0.9em";
        stats.style.color = "#666";
	if (d.voltage_v < 70)
		stats.textContent = "unpowered";
	else if (!d.active_power_w)
		stats.textContent = "OFF";
	else
		stats.textContent = `${d.active_power_w}W`;

        header.appendChild(stats);
    }

    pconDiv.appendChild(header);

    /* Context menu for PCON */
    const menuItems = [
        { label: `<b>PCON:</b> ${san(pcon.name)}` }
    ];

    if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
        if (isActuallyOn) {
            menuItems.push({
                label: "Turn Off",
                callback: () => {
                    const msg = {
                        schema: "com.warmcat.sai.pcon_control",
                        pcon_name: pcon.name,
                        on: 0
                    };
                    sai.send(JSON.stringify(msg));
                }
            });
        } else {
             menuItems.push({
                label: "Turn On",
                callback: () => {
                    const msg = {
                        schema: "com.warmcat.sai.pcon_control",
                        pcon_name: pcon.name,
                        on: 1
                    };
                    sai.send(JSON.stringify(msg));
                }
            });
        }
    }

    header.addEventListener("contextmenu", function(event) {
        if (!authd) return;
        createContextMenu(event, menuItems);
    });

    const childrenDiv = document.createElement("div");
    childrenDiv.className = "pcon-children";
    pconDiv.appendChild(childrenDiv);

    return pconDiv;
}

let last_renderPconHierarchy_state = "";

function renderPconHierarchy(container) {
    if (!container) return;

    const builders_no_time = last_builder_list.map(b => {
        const { last_seen, ...rest } = b;
        return rest;
    });

    const clean_pcons = {};
    for (const [k, p] of Object.entries(pcon_topology)) {
        const { children, ...rest } = p;
        clean_pcons[k] = rest;
    }

    /* Serialize the inputs to quickly see if we actually need to redraw everything */
    const currentState = JSON.stringify({
        pcons: clean_pcons,
        builders: builders_no_time
    });

    if (currentState === last_renderPconHierarchy_state) {
        return;
    }
    last_renderPconHierarchy_state = currentState;

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

    const appendBuilderRows = (buildersList, tbody) => {
        let prevBaseName = null;
        let prevTr = null;
        let stackedCount = 0;

        buildersList.forEach(b => {
            let baseName = b.name.split('.')[0].replace(/-\d+$/, '');
            if (!b.online && baseName === prevBaseName && prevTr) {
                stackedCount++;
                const tdInfo = prevTr.querySelector(".builder-info");
                const builderDiv = createBuilderDiv(b);
                
                /* Stacking visual effect */
                builderDiv.style.position = "absolute";
                builderDiv.style.top = (stackedCount * 6) + "px";
                builderDiv.style.left = (stackedCount * 6) + "px";
                builderDiv.style.zIndex = 10 - stackedCount;
                builderDiv.style.boxShadow = "-2px -2px 4px rgba(0,0,0,0.15)";
                
                tdInfo.style.position = "relative";
                tdInfo.style.paddingBottom = (stackedCount * 6) + "px";
                tdInfo.style.paddingRight = (stackedCount * 6) + "px";

                tdInfo.appendChild(builderDiv);
            } else {
                const tr = createBuilderRow(b);
                tbody.appendChild(tr);
                prevTr = tr;
                prevBaseName = baseName;
                stackedCount = 0;

                const builderDiv = tr.querySelector(".builder-info .ibuil");
                if (builderDiv) {
                    builderDiv.style.position = "relative";
                    builderDiv.style.zIndex = 10;
                }
            }
        });
    };

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
            appendBuilderRows(myBuilders, tbody);
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
        appendBuilderRows(orphanBuilders, tbody);
        childrenContainer.appendChild(table);

        container.appendChild(orphanDiv);
    }
}

let sai_power_samples = [];
let sai_max_total_power_w = 0;

function updatePowerGraph(total_w) {
    const overviewDiv = document.getElementById("sai_power_overview");
    const powerText = document.getElementById("sai_total_power");
    const canvas = document.getElementById("sai_power_graph");

    if (!overviewDiv || !powerText || !canvas) return;

    if (overviewDiv.classList.contains("hidden")) {
        overviewDiv.classList.remove("hidden");
    }

    /* if no updates passed (just drawing from memory), avoid adding a new sample */
    if (total_w !== null) {
        powerText.textContent = total_w + "W";

        if (total_w > sai_max_total_power_w) {
            sai_max_total_power_w = total_w;
        }

        sai_power_samples.push(total_w);
    } else {
        if (sai_power_samples.length > 0)
             powerText.textContent = sai_power_samples[sai_power_samples.length - 1] + "W";
    }

    const ctx = canvas.getContext("2d");
    if (canvas.width !== canvas.clientWidth) {
        canvas.width = canvas.clientWidth;
    }

    const w = canvas.width;
    const h = canvas.height;

    if (sai_power_samples.length > w) {
        sai_power_samples.shift();
    }

    ctx.clearRect(0, 0, w, h);
    if (sai_power_samples.length === 0) return;

    const max_y = sai_max_total_power_w > 0 ? sai_max_total_power_w : 1;

    ctx.fillStyle = "#ff4136";
    const startX = w - sai_power_samples.length;

    for (let i = 0; i < sai_power_samples.length; i++) {
        const val = sai_power_samples[i];
        const barH = (val / max_y) * h;
        ctx.fillRect(startX + i, h - barH, 1, barH);
    }
}

function ws_open_sai()
{
	var s = "", q, qa, qi, q5, q5s;

	if (document.getElementById("apirev"))
		document.getElementById("apirev").innerHTML = "API rev " + SAI_JS_API_VERSION;

	q = window.location.href;
	console.log(q);
	qi = q.indexOf("/git/");
	if (qi !== -1) {
		/* it has the /git/... does it have the project? */
		s += "/specific";
		q5 = q.substring(qi + 5);
		console.log("q5 = " + q5);
		q5s = q5.indexOf("/");
		if (q5s !== -1)
			s += "/" + q5.substring(0, q5s);
		else
			s += "/" + q5;

		/*
		 * gitohashi has ?h=branch and ?id=hash possible
		 */
		qa = q.split("?");
		if (qa[1])
			s += "?" + qa[1];
		console.log(s);
		gitohashi_integ = 1;
	}

	qi = q.indexOf("?task=");
	if (qi != -1) {
		/*
		 * it's a sai task details page
		 */
		s += "/specific?task=" + q.substring(qi + 6);
	}

	var s1 = get_appropriate_ws_url() + "/sai/browse" + s;
	if (typeof gitohashi_integ === 'undefined' || !gitohashi_integ) {
		if (s1.indexOf('?') !== -1)
			s1 += "&client=sai";
		else
			s1 += "?client=sai";
	}
//	if (s1.split("?"))
//	s1 = s1.split("?")[0];
	console.log(s1);
	sai = new WebSocket(s1, "com-warmcat-sai");
	try {

		sai.onopen = function() {
			if (typeof window.overlayTimeout !== 'undefined' && window.overlayTimeout) {
				clearTimeout(window.overlayTimeout);
				window.overlayTimeout = null;
			}
			var overlay = document.querySelector(".overlay");
			if (overlay) {
				overlay.parentNode.removeChild(overlay);
			}
			document.body.classList.remove("overlay-active");

			let savedRightPaneFlex = localStorage.getItem('sai-right-pane-flex');
			let initialVisible = 0;
			if (savedRightPaneFlex && parseInt(savedRightPaneFlex.replace(/[^0-9-]/g, '')) > 0) {
				initialVisible = 1;
			}
			sai.send(JSON.stringify({ schema: "com.warmcat.sai.builder_visibility", visible: initialVisible }));

			var par = new URLSearchParams(window.location.search),
				tid, eid, run_idx;
			tid = par.get('task');
			eid = par.get('event');
			run_idx = par.get('run');

			if (tid) {
				 console.log("tid " + tid);
				 selected_task_uuid = tid;
				 if (run_idx) window.current_task_run = run_idx;

				 var req = "{\"schema\":" +
					  "\"com.warmcat.sai.taskinfo\"," +
					  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
					  "\"logs\": 1," +
					  "\"last_log_ts\":" + last_log_timestamp + ",";
				 if (run_idx)
					 req += "\"run\":" + run_idx + ",";
				 else
					 req += "\"run\": -1,";
				 req += "\"task_hash\":" + JSON.stringify(tid) + "}";
				 sai.send(req);

				 // Also request the overview
				 sai.send("{\"schema\":" +
					  "\"com.warmcat.sai.taskinfo\", \"js_api_version\": " + SAI_JS_API_VERSION +
					  ", \"offset\": " + current_overview_offset + "}");
				 return;
			}

			if (eid) {
				 console.log("eid " + eid);
				 selected_event_uuid = eid;

				 sai.send("{\"schema\":" +
					  "\"com.warmcat.sai.eventinfo\"," +
					  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
					  "\"event_hash\":" +
					  JSON.stringify(eid) + "}");

				 // Also request the overview
				 sai.send("{\"schema\":" +
					  "\"com.warmcat.sai.taskinfo\", \"js_api_version\": " + SAI_JS_API_VERSION +
					  ", \"offset\": " + current_overview_offset + "}");
				 return;
			}

			/*
			 * request the overview schema
			 */

			 sai.send("{\"schema\":" +
				  "\"com.warmcat.sai.taskinfo\", \"js_api_version\": " + SAI_JS_API_VERSION +
				  ", \"offset\": " + current_overview_offset + "}");
		};

		sai.onmessage = function got_packet(msg) {
			var u, ci, n;
			var now_ut = Math.round((new Date().getTime() / 1000));

		//	console.log(msg.data);
		//	if (msg.data.length < 10)
		//		return;
		try {
			jso = JSON.parse(msg.data);
		} catch {
			console.log("Bad JSON received:");
			console.log(msg.data);
			return
		}
		//	console.log(jso.schema);

			if (jso.alang) {
				var a = jso.alang.split(","), n;

				for (n = 0; n < a.length; n++) {
					var b = a[n].split(";");
					switch (b[0]) {
					case "ja":
						i18n.translator.add(JSON.parse(lang_ja));
						n = a.length;
						break;
					case "zh_TW":
					case "zh_HK":
					case "zh_SG":
					case "zh_HANT":
					case "zh-TW":
					case "zh-HK":
					case "zh-SG":
					case "zh-HANT":
						i18n.translator.add(JSON.parse(lang_zht));
						n = a.length;
						break;
					case "zh":
					case "zh_CN":
					case "zh_HANS":
					case "zh-CN":
					case "zh-HANS":
						i18n.translator.add(JSON.parse(lang_zhs));
						n = a.length;
						break;
					case "en":
					case "en_US":
					case "en-US":
						n = a.length;
						break;
					}
				}
			}

			if (jso.api_version && jso.api_version !== SAI_JS_API_VERSION) {
				console.warn(`Sai JS API version mismatch. Client: ${SAI_JS_API_VERSION}, Server: ${jso.api_version}. Reloading page.`);
				location.reload(true); // Force a hard reload
				return; // Stop processing this old message
			}

			console.log(jso.schema);

			switch (jso.schema) {

			case "com.warmcat.sai.builders":
				/* Update builder list */
				let platformsArray = (jso.platforms && Array.isArray(jso.platforms)) ? jso.platforms :
				                     (jso.builders && Array.isArray(jso.builders)) ? jso.builders : null;

				if (platformsArray) {
					last_builder_list = platformsArray;
					
					/* Ensure PCONs exist in topology even if omitted by the server payload */
					last_builder_list.forEach(b => {
					    if (b.pcon && !pcon_topology[b.pcon]) {
					        pcon_topology[b.pcon] = {
					            name: b.pcon,
					            on: 0, /* Default to off so it shows grey until we get real state */
					            type: "",
					            depends_on: "",
					            children: []
					        };
					    }

					    if (!b.online) {
					        /* Builder became inactive, remove associated task steps */
					        for (const short_name in spreadsheet_data_cache) {
					            if (b.name.startsWith(short_name)) {
					                delete spreadsheet_data_cache[short_name];
					            }
					        }
					    }
					});

					const container = document.getElementById("sai_builders");
					if (container) renderPconHierarchy(container);
				}
				break;

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

			case "com.warmcat.sai.pcon_energy":
				if (jso.items) {
					jso.items.forEach(item => {
						pcon_energy_cache[item.name] = item;
						const pconDiv = document.getElementById("pcon-" + item.name);
						if (pconDiv) {
							let header = pconDiv.querySelector(".pcon-header");
							let stats = header.querySelector(".pcon-stats");
							if (!stats) {
								stats = document.createElement("span");
								stats.className = "pcon-stats";
								stats.style.marginLeft = "10px";
								stats.style.fontSize = "0.9em";
								stats.style.color = "#666";
								header.appendChild(stats);
							}

							const d = item;
							let hasPower = false;
							if (d.voltage_v < 70)
								stats.textContent = "unpowered";
							else if (!d.active_power_w)
								stats.textContent = "OFF";
							else {
								stats.textContent = `${d.active_power_w}W`;
								hasPower = true;
							}

							let plugIcon = header.querySelector("span");
							if (plugIcon) {
								plugIcon.className = hasPower ? "pcon-on" : "pcon-off";
							}
							
							const myBuilders = last_builder_list.filter(b => b.pcon === item.name);
							let anyConnected = myBuilders.some(b => b.online === 1);
							let anyPoweringUp = myBuilders.some(b => b.powering_up === 1);
							
							if (anyConnected || anyPoweringUp || hasPower) {
								header.classList.add("pcon-header-on");
							} else {
								header.classList.remove("pcon-header-on");
							}
						}
					});
					
					let total_w = 0;
					for (const p in pcon_energy_cache) {
						if (pcon_energy_cache[p].active_power_w)
							total_w += pcon_energy_cache[p].active_power_w;
					}
					updatePowerGraph(total_w);
				}
				break;

			case "com.warmcat.sai.power_history":
				sai_max_total_power_w = jso.max_w;
				sai_power_samples = [];
				if (jso.samples) {
					for (var j = 0; j < jso.samples.length; j++) {
						sai_power_samples.push(jso.samples[j]);
					}
				}
				updatePowerGraph(null); /* Redraw instantly without new sample */
				break;

			case "com.warmcat.sai.build-metric":
				var summaryDiv = document.getElementById("metrics-summary-" + jso.task_uuid);
				if (summaryDiv) {
					var s = "<div class=\"metric-summary\">" +
						"Step Metrics: " +
						"CPU: " + (jso.us_cpu_user / 1000000).toFixed(2) + "s user, " +
						(jso.us_cpu_sys / 1000000).toFixed(2) + "s sys; " +
						"Wallclock: " + (jso.wallclock_us / 1000000).toFixed(2) + "s; " +
						"Mem: " + humanize(jso.peak_mem_rss) + "B; " +
						"Stg: " + humanize(jso.stg_bytes) + "B; " +
						"Parallel: " + jso.parallel +
						"</div>";
					summaryDiv.innerHTML += s;
				}
				break;

			case "com.warmcat.sai.watcher_services":
				watcher_services = jso;
				break;

			case "com.warmcat.sai.ptydata":
				if (jso.task_uuid) {
					let term = active_terminals[jso.task_uuid];
					if (!term) {
						let platformStr = null;
						if (typeof last_builder_list !== 'undefined' && jso.builder_name) {
							const b = last_builder_list.find(x => x.name === jso.builder_name);
							if (b) platformStr = b.platform;
						}
						term = new SaiTerminal(document.body, {
							title: "Terminal: " + (jso.builder_name || "Unknown"),
							platform: platformStr,
							onData: function(input) {
								const msg = {
									schema: "com.warmcat.sai.ptydata",
									task_uuid: jso.task_uuid,
									channel: 0,
									len: input.length,
									data: btoa(input)
								};
								sai.send(JSON.stringify(msg));
							},
							onResize: function(cols, rows) {
								const msg = {
									schema: "com.warmcat.sai.ptydata",
									task_uuid: jso.task_uuid,
									channel: 0,
									cols: cols,
									rows: rows,
									len: 0,
									data: ""
								};
								sai.send(JSON.stringify(msg));
							},
							onClose: function() {
								const msg = {
									schema: "com.warmcat.sai.closeshell",
									task_uuid: jso.task_uuid
								};
								sai.send(JSON.stringify(msg));
								delete active_terminals[jso.task_uuid];
							}
						});

						active_terminals[jso.task_uuid] = term;
					}
					if (jso.data) {
						const binString = atob(jso.data);
						const bytes = new Uint8Array(binString.length);
						for (let i = 0; i < binString.length; i++) {
							bytes[i] = binString.charCodeAt(i);
						}
						const text = new TextDecoder().decode(bytes);
						term.write(jso.channel, text);
					}
				}
				break;

			case "sai.warmcat.com.overview":
				/*
				 * Sent with an array of e[] to start, but also
				 * can send a single e[] if it just changed
				 * state
				 */
				if (jso.overview) {
					jso.overview = jso.overview.filter(o => !deleted_events_cache.has(o.e.uuid));
					if (jso.overview.length === 0 && typeof jso.total_events === 'undefined') {
						break;
					}
				}

				var old_latest_uuid = loaded_events.length ? loaded_events[loaded_events.length - 1].e.uuid : null;

				if (jso.overview) {
					jso.overview.forEach(function(new_ev) {
						var idx = loaded_events.findIndex(o => o.e.uuid === new_ev.e.uuid);
						if (idx !== -1) {
							loaded_events[idx] = new_ev;
						} else {
							loaded_events.push(new_ev);
						}
					});
					if (jso.overview.length > 1) {
						loaded_events = jso.overview;
						if (typeof jso.total_events !== 'undefined') total_events = jso.total_events;
						if (typeof jso.offset !== 'undefined') current_offset = jso.offset;
					}
				}

				if (loaded_events.length > 0) {
					if (!selected_event_uuid) {
						if (selected_task_uuid) {
							var evUuid = find_event_by_task_uuid(selected_task_uuid);
							if (evUuid) selected_event_uuid = evUuid;
						}
						if (!selected_event_uuid) {
							selected_event_uuid = loaded_events[loaded_events.length - 1].e.uuid;
						}
					}
				}
				render_event_decals();

				var new_latest_uuid = loaded_events.length ? loaded_events[loaded_events.length - 1].e.uuid : null;
				if (old_latest_uuid && new_latest_uuid !== old_latest_uuid) {
					/* A new event arrived! Select it and clear logs. */
					selectEvent(new_latest_uuid);
				} else {
					if (selected_event_uuid) {
						var ev_obj = loaded_events.find(o => o.e.uuid === selected_event_uuid);
						if (ev_obj) {
							render_selected_event_tasks(ev_obj);
						}
					}
				}

				aging();
				break;

			case "com.warmcat.sai.taskinfo":

				if (!jso.t)
					break;

				if (new URLSearchParams(window.location.search).get('task') === jso.t.uuid) {
					window.current_viewed_task_state = jso.t.state;
				}

				if (loaded_events) {
					var event_uuid = jso.t.uuid.substring(0, 32);
					var ev_obj = loaded_events.find(function(o) { return o.e.uuid === event_uuid; });
					if (ev_obj) {
						if (jso.e) {
							ev_obj.e = Object.assign({}, ev_obj.e, jso.e);
						}
						if (ev_obj.t) {
							var t_run = typeof jso.t.run !== 'undefined' ? jso.t.run : 0;
							var t_idx = ev_obj.t.findIndex(function(t) { return t.uuid === jso.t.uuid && (typeof t.run !== 'undefined' ? t.run : 0) === t_run; });
							if (t_idx !== -1) {
								ev_obj.t[t_idx] = Object.assign({}, ev_obj.t[t_idx], jso.t);
							} else {
								ev_obj.t.push(jso.t);
							}
						}
					}
				}

				if (document.getElementById("taskstate_" + jso.t.uuid)) {
					console.log("found taskstate_" + jso.t.uuid);
					refresh_state(jso.t);
					update_summary_and_progress(jso.t.uuid.substring(0, 32));
				}

				/* update task summary if shown anywhere */
				if (document.getElementById("taskinfo-" + jso.t.uuid)) {
					if (typeof window.current_task_run !== 'undefined' && window.current_task_run !== jso.t.run) {
						if (document.getElementById("sai-task-logs"))
							document.getElementById("sai-task-logs").innerHTML = "";
						/* update the URL without reloading so sharing works */
						var par = new URLSearchParams(window.location.search);
						par.set('run', jso.t.run);
						var path = window.location.pathname;
						if (!path.endsWith('/') && !path.endsWith('index.html')) {
							path += '/';
						}
						window.history.replaceState({}, '', path + '?' + par.toString());
					}
					window.current_task_run = jso.t.run;
					var ti_el = document.getElementById("taskinfo-" + jso.t.uuid);
					ti_el.className = "taskinfo taskstate" + jso.t.state;
					ti_el.innerHTML = sai_taskinfo_render(jso);
					if (jso.e) {
						if (document.getElementById("esr-" + jso.e.uuid))
							document.getElementById("esr-" + jso.e.uuid).innerHTML =
								sai_event_summary_render(jso, now_ut, 1);
						update_summary_and_progress(jso.e.uuid);
					}
				}

				if (!document.getElementById("taskstate_" + jso.t.uuid) &&
				    !document.getElementById("taskinfo-" + jso.t.uuid)) {

					console.log("NO taskinfo- or taskstate_" + jso.t.uuid);

					/*
					 * Last chance if we might be
					 * on a task-specific page, and
					 * want to show the task info
					 * at the top
					 */

					const urlParams = new URLSearchParams(window.location.search);
					const url_task_uuid = urlParams.get('task');

					if (url_task_uuid === jso.t.uuid &&
					    document.getElementById("sai_sticky")) {
						window.current_task_run = jso.t.run;
						document.getElementById("sai_sticky").innerHTML =
							"<div class=\"taskinfo taskstate" + jso.t.state + "\" id=\"taskinfo-" +
							san(jso.t.uuid) + "\">" +
							sai_taskinfo_render(jso) +
							"</div>";

						s = "<table><td colspan=\"3\"><pre><table class=\"scrollogs\"><tr>" +
						"<td class=\"atop\">" +
						"<div id=\"dlogsn\" class=\"dlogsn\">" + lines + "</div></td>" +
						"<td class=\"atop\">" +
						"<div id=\"dlogst\" class=\"dlogst\">" + times + "</div></td>" +
						 "<td class=\"atop\"><div id=\"dlogs\" class=\"dlogs\">" +
						 "<span id=\"logs\" class=\"nowrap\">" + logs +
						"</span>"+
						"</div></td></tr></table></pre>";

						if (document.getElementById("sai_overview")) {
							document.getElementById("sai_overview").innerHTML = s;
							logs_pending = times_pending = lines_pending = "";

							if (jso.e && document.getElementById("esr-" + jso.e.uuid))
								document.getElementById("esr-" + jso.e.uuid).innerHTML =
									sai_event_summary_render(jso, now_ut, 1);

						}
					}
				}
				if (jso.e)
					update_summary_and_progress(jso.e.uuid);

				if (document.getElementById("rebuild-" + san(jso.t.uuid))) {
					document.getElementById("rebuild-" + san(jso.t.uuid)).
						addEventListener("click", function(e) {
							var rs= "{\"schema\":" +
							 "\"com.warmcat.sai.taskreset\"," +
							 "\"uuid\": " +
								JSON.stringify(san(e.srcElement.id.substring(8))) + "}";

							console.log(rs);
							sai.send(rs);

							var tid = san(e.srcElement.id.substring(8));
							if (new URLSearchParams(window.location.search).get('run')) {
								window.location.search = '?task=' + tid;
								return;
							}

							/*
							 * and immediately re-request the task info, so we can get
							 * the new logs
							 */
							var rq = "{\"schema\":" +
								  "\"com.warmcat.sai.taskinfo\"," +
								  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
								  "\"logs\": 1," +
								  "\"run\": -1," +
								  "\"last_log_ts\":" + last_log_timestamp + "," +
								  "\"task_hash\":" +
								  JSON.stringify(tid) + "}";

							console.log(rq);
							sai.send(rq);

							document.getElementById("dlogsn").innerHTML = "";
							document.getElementById("dlogst").innerHTML = "";
							document.getElementById("dlogs").innerHTML = "<span id=\"logs\" class=\"nowrap\"></span>";
							lines = times = logs = "";
							lines_pending = times_pending = logs_pending = "";
							segment_stack = [];
							seg_counter = 0;
							window.held_start_line = null;
							logAnsiState = {};
							tfirst = 0;
							lli = 1;
							last_log_timestamp = 0;
						});
				}

				if (document.getElementById("stop-" + san(jso.t.uuid))) {
					document.getElementById("stop-" + san(jso.t.uuid)).
						addEventListener("click", function(e) {
							var rs= "{\"schema\":" +
							 "\"com.warmcat.sai.taskcan\"," +
							 "\"task_uuid\": " +
								JSON.stringify(san(e.srcElement.id.substring(5))) + "}";
							 console.log(rs);
							sai.send(rs);
						});
				}

				aging();
				break;

			case "com.warmcat.sai.loadreport":
				// Cache the whole report for subsequent builder redraws
				loadreport_data_cache[jso.builder_name] = jso;

				const builderDiv = document.getElementById('binfo-' + jso.builder_name);
				if (builderDiv) {
					const cpuBar = builderDiv.querySelector(".res-bar-cpu");
					const ramBar = builderDiv.querySelector(".res-bar-ram");
					const diskBar = builderDiv.querySelector(".res-bar-disk");

					if (cpuBar) {
						let cpu_percentage = jso.cpu_percent / 10;
						if (cpu_percentage > 100) cpu_percentage = 100;
						if (cpu_percentage < 0) cpu_percentage = 0;
						let width_class = `w-${Math.round(cpu_percentage / 5) * 5}`;

						cpuBar.classList.forEach(c => { if (c.startsWith('w-')) cpuBar.classList.remove(c); });
						cpuBar.classList.add(width_class);
					}
					if (ramBar) {
						let ram_percentage = 0;
						if (jso.initial_free_ram_kib > 0) {
							ram_percentage = (jso.reserved_ram_kib / jso.initial_free_ram_kib) * 100;
						}
						if (ram_percentage > 100) ram_percentage = 100;
						if (ram_percentage < 0) ram_percentage = 0;

						let width = Math.round(ram_percentage / 5) * 5;
						if (width === 0 && ram_percentage > 0)
							width = 5;

						let width_class = `w-${width}`;

						ramBar.classList.forEach(c => { if (c.startsWith('w-')) ramBar.classList.remove(c); });
						ramBar.classList.add(width_class);
					}
					if (diskBar) {
						let disk_percentage = 0;
						if (jso.initial_free_disk_kib > 0) {
							disk_percentage = (jso.reserved_disk_kib / jso.initial_free_disk_kib) * 100;
						}
						if (disk_percentage > 100) disk_percentage = 100;
						if (disk_percentage < 0) disk_percentage = 0;

						let width = Math.round(disk_percentage / 5) * 5;
						if (width === 0 && disk_percentage > 0)
							width = 5;

						let width_class = `w-${width}`;

						diskBar.classList.forEach(c => { if (c.startsWith('w-')) diskBar.classList.remove(c); });
						diskBar.classList.add(width_class);
					}
				}

				// Part 2: Update the spreadsheet of active tasks for the builder
				if (jso.active_tasks && jso.active_tasks.length > 0)
					spreadsheet_data_cache[jso.builder_name] = jso.active_tasks;
				else
					delete spreadsheet_data_cache[jso.builder_name];

				const spreadsheetContainer = document.getElementById('spreadsheet-' + jso.builder_name);
				if (spreadsheetContainer) {
					updateSpreadsheetDOM(spreadsheetContainer, spreadsheet_data_cache[jso.builder_name]);
					if (spreadsheet_data_cache[jso.builder_name]) {
						aging();
					}
				}
				break;

			case "com-warmcat-sai-artifact":
				console.log(jso);

				sai_arts += "<div class=\"sai_arts\"><img src=\"artifact.svg\">&nbsp;<a href=\"artifacts/" +
					san(jso.task_uuid) + "/" +
					san(jso.artifact_down_nonce) + "/" +
					san(jso.blob_filename) + "\">" +
					san(jso.blob_filename) + "</a>&nbsp;" +
					humanize(jso.len) + "B </div>";

				if (document.getElementById("sai_arts"))
					document.getElementById("sai_arts").innerHTML = sai_arts;

				break;

			case "com.warmcat.sai.taskactivity":
				ongoing_task_activities = {};
				if (jso.activity) {
					for (var i = 0; i < jso.activity.length; i++) {
						var act = jso.activity[i];
						ongoing_task_activities[act.uuid] = act.cat;
					}
				} else
						console.log("no spreadsheetContainer");
				break;

			case "com.warmcat.sai.unauthorized":
				location.reload();
				break;

			case "com.warmcat.sai.auth_state":
				console.log("Backend auth_state:", jso.auth_state);
				if (jso.auth_state === 3) {
					auth_state = SaiAuthState.LOGGED_IN_GRANT_ADMIN;
					auth_is_admin = 1;
				} else if (jso.auth_state === 2) {
					auth_state = SaiAuthState.LOGGED_IN_GRANT_USER;
					auth_is_admin = 0;
				} else if (jso.auth_state === 1) {
					auth_state = SaiAuthState.LOGGED_IN_NO_GRANT;
					auth_is_admin = 0;
				} else {
					auth_state = SaiAuthState.NOT_LOGGED_IN;
					auth_is_admin = 0;
				}
				const statusContainer = document.getElementById('lws-login-status-container');
				if (statusContainer) {
					statusContainer.classList.remove('grant-admin', 'grant-user', 'grant-none');
					if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
						statusContainer.classList.add('grant-admin');
					} else if (auth_state === SaiAuthState.LOGGED_IN_GRANT_USER) {
						statusContainer.classList.add('grant-user');
					} else if (auth_state === SaiAuthState.LOGGED_IN_NO_GRANT) {
						statusContainer.classList.add('grant-none');
					}
				}
				break;

			case "com.warmcat.sai.event_deleted":
				window.location.href = window.location.origin + window.location.pathname;
				break;

			case "com-warmcat-sai-logs":
				var s1;
				try {
					var binString = atob(jso.log);
					if (window.TextDecoder) {
						window._sai_text_decoder = window._sai_text_decoder || new TextDecoder("utf-8");
						var bytes = new Uint8Array(binString.length);
						for (var i = 0; i < binString.length; i++) {
							bytes[i] = binString.charCodeAt(i);
						}
						s1 = window._sai_text_decoder.decode(bytes, {stream: true});
					} else {
						s1 = decodeURIComponent(escape(binString));
					}
				} catch (e) {
					console.log("decode err", e);
					break;
				}

				if (window._sai_ansi_buffer) {
					s1 = window._sai_ansi_buffer + s1;
					window._sai_ansi_buffer = "";
				}

				var last_esc = s1.lastIndexOf('\u001b');
				if (last_esc >= 0) {
					var tail = s1.substring(last_esc);
					var is_complete = true;
					if (tail.startsWith('\u001b[')) {
						is_complete = /(\u001b\[[0-9:;<=>?]*[ -/]*[@-~])/.test(tail);
					} else if (tail.startsWith('\u001b]')) {
						is_complete = tail.indexOf('\x07') !== -1 || tail.indexOf('\u001b\\', 1) !== -1;
					} else if (tail === '\u001b') {
						is_complete = false;
					}
					
					if (!is_complete && tail.length < 512) {
						window._sai_ansi_buffer = tail;
						s1 = s1.substring(0, last_esc);
					}
				}

				if (!tfirst) tfirst = jso.timestamp;
				last_log_timestamp = jso.timestamp;

				/* normalize CRs to LFs so line number counts track them properly */
				if (window._sai_cr_pending && s1.startsWith('\n')) {
					s1 = s1.substring(1);
				}
				window._sai_cr_pending = s1.endsWith('\r');
				s1 = s1.replace(/\r\n/g, '\n').replace(/\r/g, '\n');

				var lines_started = 0;
				var lines_arr = s1.split('\n');
				for (var idx = 0; idx < lines_arr.length; ++idx) {
					if (idx === lines_arr.length - 1 && lines_arr[idx] === '') continue;
					
					var text_line = lines_arr[idx];
					var has_nl = (idx < lines_arr.length - 1) ? '\n' : '';
					var line_str = text_line + has_nl;
					
					var ansiResult = ansiToHtml(line_str, logAnsiState);
					var s = ansiResult.html;
					logAnsiState = ansiResult.newState;
					
					var li = has_nl ? 1 : 0;
					var en = "", tn = "";
					if (cont && !cont[jso.channel] && jso.len)
						tn = ((jso.timestamp - tfirst) / 1000000).toFixed(4);

					var temp_li = li;
					var temp_lli = lli;
					while (temp_li > 0) {
						en += "<a id=\"#sn" + temp_lli + "\" href=\"#sn" + temp_lli + "\">" + temp_lli + "</a><br>";
						tn += "<br>";
						temp_lli++;
						temp_li--;
					}
					
					var s_logs = "";
					switch (jso.channel) {
					case 1: s_logs = s; break;
					case 2: s_logs = "<span class=\"stderr\">" + s + "</span>"; break;
					case 3: s_logs = "<span class=\"saibuild\">\u{25a0} " + s + "</span>"; break;
					case 4: s_logs = "<span class=\"tty0\">" + s + "</span>"; break;
					default: s_logs = "<span class=\"tty1\">" + s + "</span>"; break;
					}

					var eval_line = text_line;
					if (window.pending_log_line && idx === 0) {
						eval_line = window.pending_log_line + text_line;
					}
					
					if (has_nl === '') {
						window.pending_log_line = eval_line;
					} else if (idx === 0) {
						window.pending_log_line = "";
					}

					var skip_push = false;
					var skip_render = false;
					var match_fail = (jso.channel === 1 || jso.channel === 2) ? eval_line.match(/Test\s+#(\d+):\s+.*(Failed|\*\*\*|Timeout)/i) : null;
					var is_fail = match_fail || ((jso.channel === 1 || jso.channel === 2) && /test failed/i.test(eval_line));
					
					if (jso.channel === 1 || jso.channel === 2) {
						if (window.held_start_line) {
							if (is_fail) {
								var is_same_test = false;
								if (match_fail) {
									var start_match = window.held_start_line.text.match(/Start\s+(\d+):/i);
									if (start_match && start_match[1] === match_fail[1]) {
										is_same_test = true;
									}
								}
								// If the failing test isn't the one that just started, it must be running in parallel.
								// Flush the unrelated valid 'Start' line out into the parent CTest boundary first.
								if (!is_same_test) {
									logs += window.held_start_line.s_logs; logs_pending += window.held_start_line.s_logs;
									if (window.held_start_line.li) {
										lines += window.held_start_line.en; lines_pending += window.held_start_line.en;
										times += window.held_start_line.tn; times_pending += window.held_start_line.tn;
									}
									window.held_start_line = null;
								}
							
								while (segment_stack.length > 1) pop_segment();
								push_segment(eval_line, true);
								skip_push = true;
							}
							
							if (window.held_start_line) {
								logs += window.held_start_line.s_logs; logs_pending += window.held_start_line.s_logs;
								if (window.held_start_line.li) {
									lines += window.held_start_line.en; lines_pending += window.held_start_line.en;
									times += window.held_start_line.tn; times_pending += window.held_start_line.tn;
								}
								window.held_start_line = null;
							}
						}
						
						if (/^\s*Start\s+\d+:/i.test(eval_line)) {
							window.held_start_line = { text: eval_line, s_logs: s_logs, en: en, tn: tn, li: li };
							skip_render = true;
						}
					}

					if (jso.channel === 3) {
						if (/^>saib>\s+Starting task step/.test(eval_line)) {
							while (segment_stack.length > 0) pop_segment();
							push_segment(eval_line, true);
						} else if (/^>saib>\s+Step \d+:/.test(eval_line) && segment_stack.length > 0) {
							var pseg = segment_stack[0];
							var phdr = document.getElementById("hdr-seg-" + pseg.id);
							if (phdr) phdr.querySelector('.seg-title').innerText = eval_line;
						} else {
							while (segment_stack.length > 1) pop_segment();
						}
					}
					
					if (jso.channel === 1 || jso.channel === 2) {
						if (skip_push) {
							// Fold logic successfully handled during lookahead execution
						} else if (match_fail || /test failed/i.test(eval_line)) {
							while (segment_stack.length > 1) pop_segment();
							push_segment(eval_line, true);
						} else if (/^\d+% tests passed/i.test(eval_line) || /Total Test time/i.test(eval_line) || /The following tests FAILED:/i.test(eval_line) || /Errors while running CTest/i.test(eval_line)) {
							while (segment_stack.length > 1) pop_segment();
						} else if (/^\d+\/\d+\s+Test\s+#\d+:/i.test(eval_line)) {
							while (segment_stack.length > 1) pop_segment();
						}
					}
					
					var text_lower = eval_line.toLowerCase();
					if (is_fail || text_lower.includes("error:") || text_lower.includes("fatal:") || /error\s+[a-z0-9_]+:/i.test(text_lower)) {
						for (var si = 0; si < segment_stack.length; si++) {
							var sobj = segment_stack[si];
							sobj.error_count++;
							if (si < segment_stack.length - 1) {
								var sbody = document.getElementById("seg-" + sobj.id);
								var shdr = document.getElementById("hdr-seg-" + sobj.id);
								if (sbody && shdr && sbody.classList.contains("hide")) {
									sbody.classList.remove("hide");
									var sicon = shdr.querySelector('.fold-icon');
									if (sicon) sicon.innerText = "▼";
								}
							}
						}
					} else if (text_lower.includes("warning:")) {
						for (var si = 0; si < segment_stack.length; si++) segment_stack[si].warning_count++;
					}

					if (segment_stack.length > 0) {
						segment_stack[segment_stack.length - 1].lines_count += (has_nl ? 1 : 0);
					}

					if (!skip_render) {
						if (s_logs) {
							logs += s_logs; logs_pending += s_logs;
						}
						if (li) {
							lines += en; lines_pending += en;
							times += tn; times_pending += tn;
						}
					}

					if (cont)
						cont[jso.channel] = (li === 0);

					while (li > 0) {
						lli++;
						li--;
					}

				}

				if (!redpend) {
					redpend = 1;
					setTimeout(function() {
						const rightPane = document.getElementById('sai_overview') || document.querySelector('.right-pane');
						redpend = 0;
						if (rightPane)
							locked = rightPane.scrollHeight -
								rightPane.clientHeight <=
								rightPane.scrollTop + 1;

						if (locked) {
							for (var si = 0; si < segment_stack.length; si++) {
								var sobj = segment_stack[si];
								var sdom = document.getElementById("seg-" + sobj.id);
								var hdom = document.getElementById("hdr-seg-" + sobj.id);
								if (sdom && sdom.classList.contains("hide")) {
									sdom.classList.remove("hide");
									sobj.auto_unfolded = true;
									if (hdom) {
										var icon = hdom.querySelector('.fold-icon');
										if (icon) icon.innerText = "▼";
									}
								}
							}
						}

						flush_segments();
						check_and_apply_failure_ui();

						if (locked && rightPane)
						   rightPane.scrollTop =
							rightPane.scrollHeight -
							rightPane.clientHeight;
					}, 500);
				}

		break;
	} /* switch */
	} /* onmessage */
		sai.onerror = function(ev) {
			console.log("WebSocket error:", ev);
		};

		sai.onclose = function(ev){
			console.log("WebSocket closed. Code:", ev.code, "Reason:", ev.reason);
			
			if (typeof overlayTimeout !== 'undefined' && overlayTimeout) clearTimeout(overlayTimeout);
			
			window.overlayTimeout = setTimeout(function() {
				if (!document.querySelector(".overlay")) {
					var overlay = document.createElement("div");
					overlay.className = "overlay";
					document.body.appendChild(overlay);
					document.body.classList.add("overlay-active");
				}
			}, 3000);

			myVar = setTimeout(ws_open_sai, 1000);
		};
	} catch(exception) {
		alert("<p>Error" + exception);
	}
}

/* stuff that has to be delayed until all the page assets are loaded */

window.addEventListener("load", function() {

	document.addEventListener('click', function(e) {
		var hdr = e.target.closest('.log-segment-header');
		if (hdr) {
			var id = hdr.id.substring(8);
			toggleSegment(id);
		}
		var pbtn = e.target.closest('.sai-pagination-btn');
		if (pbtn) {
			if (window.change_page) {
				window.change_page(parseInt(pbtn.getAttribute('data-offset')));
			}
		}

		var a = e.target.closest('a');
		if (a) {
			var href = a.getAttribute('href');
			if (href && (href.indexOf('?task=') !== -1 || href.indexOf('index.html?task=') !== -1)) {
				e.preventDefault();
				var urlParams = new URLSearchParams(href.substring(href.indexOf('?')));
				var taskUuid = urlParams.get('task');
				var runVal = urlParams.get('run') || '-1';
				selectTask(taskUuid, runVal);
			}
		}

		var rebuildBtn = e.target.closest("[id^='rebuild-']");
		if (rebuildBtn && !rebuildBtn.id.startsWith("rebuild-ev-")) {
			var tid = rebuildBtn.id.substring(8);
			var rs = "{\"schema\":\"com.warmcat.sai.taskreset\",\"uuid\":" + JSON.stringify(san(tid)) + "}";
			console.log(rs);
			sai.send(rs);

			// Clear logs and re-request taskinfo
			var dlogsn = document.getElementById("dlogsn");
			var dlogst = document.getElementById("dlogst");
			var dlogs = document.getElementById("dlogs");
			if (dlogsn) dlogsn.innerHTML = "";
			if (dlogst) dlogst.innerHTML = "";
			if (dlogs) dlogs.innerHTML = "<span id=\"logs\" class=\"nowrap\"></span>";
			lines = times = logs = "";
			lines_pending = times_pending = logs_pending = "";
			segment_stack = [];
			seg_counter = 0;
			window.held_start_line = null;
			logAnsiState = {};
			tfirst = 0;
			lli = 1;
			last_log_timestamp = 0;

			var rq = "{\"schema\":\"com.warmcat.sai.taskinfo\",\"js_api_version\":" + SAI_JS_API_VERSION + ",\"logs\":1,\"run\":-1,\"last_log_ts\":" + last_log_timestamp + ",\"task_hash\":" + JSON.stringify(tid) + "}";
			console.log(rq);
			sai.send(rq);
		}

		var stopBtn = e.target.closest("[id^='stop-']");
		if (stopBtn) {
			var tid = stopBtn.id.substring(5);
			var rs = "{\"schema\":\"com.warmcat.sai.taskcan\",\"task_uuid\":" + JSON.stringify(san(tid)) + "}";
			console.log(rs);
			sai.send(rs);
		}

		var rebuildEvBtn = e.target.closest("[id^='rebuild-ev-']");
		if (rebuildEvBtn) {
			var uuid = rebuildEvBtn.id.substring(11);
			var rs = "{\"schema\":\"com.warmcat.sai.eventreset\",\"uuid\":" + JSON.stringify(san(uuid)) + "}";
			console.log(rs);
			sai.send(rs);
		}

		var deleteEvBtn = e.target.closest("[id^='delete-ev-']");
		if (deleteEvBtn) {
			var uuid = deleteEvBtn.id.substring(10);
			deleted_events_cache.add(uuid);
			var rs = "{\"schema\":\"com.warmcat.sai.eventdelete\",\"uuid\":" + JSON.stringify(uuid) + "}";
			console.log(rs);
			sai.send(rs);

			// Remove / hide the decal card
			var card = document.querySelector(".event-decal-card[data-uuid='" + uuid + "']");
			if (card) {
				card.style.transition = 'opacity 0.3s';
				card.style.opacity = '0';
				setTimeout(function() {
					if (card.parentNode) {
						card.parentNode.removeChild(card);
					}
				}, 300);
			}

			// Clean up selected state if the deleted event was currently selected
			if (selected_event_uuid === uuid) {
				var active_events = loaded_events.filter(o => !deleted_events_cache.has(o.e.uuid));
				if (active_events.length > 0) {
					var next_selected_uuid = active_events[active_events.length - 1].e.uuid;
					selectEvent(next_selected_uuid);
				} else {
					selected_event_uuid = null;
					selected_task_uuid = null;
					window.current_task_run = null;

					var tasksSection = document.getElementById("sai_event_tasks");
					if (tasksSection) {
						tasksSection.innerHTML = "<div class=\"event-tasks-header\"><span class=\"event-tasks-title\">No event selected</span></div>";
					}

					var stickyEl = document.getElementById("sai_sticky");
					var overviewEl = document.getElementById("sai_overview");
					if (stickyEl) stickyEl.innerHTML = "";
					if (overviewEl) overviewEl.innerHTML = "";

					var par = new URLSearchParams(window.location.search);
					par.delete("task");
					par.delete("run");
					par.delete("event");
					var qs = par.toString();
					var path = window.location.pathname;
					if (!path.endsWith('/') && !path.endsWith('index.html')) {
						path += '/';
					}
					window.history.pushState({}, "", path + (qs ? ("?" + qs) : ""));
				}
			}
		}
	});

	const savedFlex = localStorage.getItem('sai-right-pane-flex');
	if (savedFlex) {
		const rightPane = document.querySelector('.right-pane');
		if (rightPane) {
			rightPane.style.flex = savedFlex;
		}
	}

	const lnameInput = document.getElementById("lname");
	const lpassInput = document.getElementById("lpass");

	function stopClickPropagation(event) {
		// This is the key. It prevents the click event from
		// reaching any parent elements.
		event.stopPropagation();
	}

	if (lnameInput) {
		lnameInput.addEventListener("click", stopClickPropagation);
	}

	if (lpassInput) {
		lpassInput.addEventListener("click", stopClickPropagation);
	}

	if (document.getElementById("noscript"))
		document.getElementById("noscript").display = "none";

	/* LWS Login hook */
	var loginPromise = Promise.resolve();
	if (window.renderLwsLoginStatus)
		loginPromise = window.renderLwsLoginStatus('lws-login-status-container');

	loginPromise.then(function() {
		return fetch('.lws-login-status');
	})
	.then(function(res) { return res.json(); })
	.then(function(data) {
		console.log("LOGIN STATUS DEBUG:", data);
		auth_state = SaiAuthState.NOT_LOGGED_IN;
		if (data.logged_in) {
			if (data.has_grant) {
				authd = 1;
				auth_grant_level = data.grant_level !== undefined ? data.grant_level : -1;
				const isAdmin = data.is_admin === true || data.is_admin === 1 || data.is_admin === "true" || data.is_admin === "1";
				if (auth_grant_level >= 2 || isAdmin) {
					auth_state = SaiAuthState.LOGGED_IN_GRANT_ADMIN;
					auth_is_admin = 1;
				} else {
					auth_state = SaiAuthState.LOGGED_IN_GRANT_USER;
				}
			} else {
				auth_state = SaiAuthState.LOGGED_IN_NO_GRANT;
			}

			const container = document.getElementById('lws-login-status-container');
			if (container) {
				container.classList.remove('grant-admin', 'grant-user', 'grant-none');
				if (auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
					container.classList.add('grant-admin');
				} else if (auth_state === SaiAuthState.LOGGED_IN_GRANT_USER) {
					container.classList.add('grant-user');
				} else if (auth_state === SaiAuthState.LOGGED_IN_NO_GRANT) {
					container.classList.add('grant-none');
				}
			}
		}
	})
	.catch(function(err) {
		console.log('lws-login auth fetch failed: ', err);
	})
	.finally(function() {
		ws_open_sai();
		aging();
	});

	setInterval(function() {
		update_task_activities();

	    var locked = document.body.scrollHeight -
		document.body.clientHeight <= document.body.scrollTop + 1;

	    if (locked)
	     document.body.scrollTop = document.body.scrollHeight -
		document.body.clientHeight;

	}, 500)

	document.addEventListener("contextmenu", function(event) {
		let target = event.target;
		let taskDiv = null;

		// find the taskstate div parent
		while (target && target !== document.body) {
			if (target.classList && target.classList.contains("taskstate")) {
				taskDiv = target;
				break;
			}
			target = target.parentElement;
		}

		if (taskDiv && auth_state === SaiAuthState.LOGGED_IN_GRANT_ADMIN) {
			event.preventDefault();

			const taskUuid = taskDiv.id.substring(10);
			const eventUuid = taskDiv.dataset.eventUuid;
			const platform = taskDiv.dataset.platform;

			const menuItems = [
				{
					label: "Rebuild this task",
					callback: () => {
						sai.send(JSON.stringify({
							schema: "com.warmcat.sai.taskreset",
							uuid: taskUuid
						}));
					}
				},
				{
					label: "Remove all tries",
					callback: () => {
						sai.send(JSON.stringify({
							schema: "com.warmcat.sai.taskremovealltries",
							uuid: taskUuid
						}));
					}
				},
				{
					label: `Rebuild all <b>${hsanitize(platform)}</b>`,
					callback: () => {
						sai.send(JSON.stringify({
							schema: "com.warmcat.sai.platreset",
							event_uuid: eventUuid,
							platform: platform
						}));
					}
				}
			];

			const isFinalState = ["taskstate3", "taskstate4", "taskstate5", "taskstate7"].some(s => taskDiv.classList.contains(s));

			if (!isFinalState) {
				if (taskDiv.classList.contains("taskstate10")) {
					menuItems.push({
						label: "Continue task",
						callback: () => {
							sai.send(JSON.stringify({
								schema: "com.warmcat.sai.taskresume",
								uuid: taskUuid
							}));
						}
					});
				} else {
					menuItems.push({
						label: "Pause task",
						callback: () => {
							sai.send(JSON.stringify({
								schema: "com.warmcat.sai.taskpause",
								uuid: taskUuid
							}));
						}
					});
					menuItems.push({
						label: "Kill task",
						callback: () => {
							sai.send(JSON.stringify({
								schema: "com.warmcat.sai.taskkill",
								uuid: taskUuid
							}));
						}
					});
				}
			}

			if (taskDiv.dataset.rebuildable === "1")
				menuItems.splice(1, 0, {
					label: "Rebuild last step",
					callback: () => {
						sai.send(JSON.stringify({
							schema: "com.warmcat.sai.taskrebuildlaststep",
							uuid: taskUuid
						}));
					}
				});

			createContextMenu(event, menuItems);
		}
	});
	const resizer = document.getElementById('resizer');
	if (resizer) {
		const rightPane = resizer.nextElementSibling;

		let x = 0;
		let rightWidth = 0;
		let lastVisible = -1;

		const onMouseMove = (e) => {
			const dx = x - e.clientX;
			let newRightWidth = rightWidth + dx;
			if (newRightWidth < 20) newRightWidth = 0;
			rightPane.style.flex = `0 0 ${newRightWidth}px`;
			let visible = newRightWidth > 0 ? 1 : 0;
			if (visible !== lastVisible) {
				lastVisible = visible;
				sai.send(JSON.stringify({ schema: "com.warmcat.sai.builder_visibility", visible: visible }));
			}
		};

		const onMouseUp = () => {
			document.removeEventListener('mousemove', onMouseMove);
			document.removeEventListener('mouseup', onMouseUp);
			localStorage.setItem('sai-right-pane-flex', rightPane.style.flex);
		};

		const onMouseDown = (e) => {
			x = e.clientX;
			rightWidth = rightPane.getBoundingClientRect().width;
			lastVisible = rightWidth > 0 ? 1 : 0;
			document.addEventListener('mousemove', onMouseMove);
			document.addEventListener('mouseup', onMouseUp);
		};

		const onTouchMove = (e) => {
			if (e.touches.length === 1) {
				const dx = x - e.touches[0].clientX;
				let newRightWidth = rightWidth + dx;
				if (newRightWidth < 20) newRightWidth = 0;
				rightPane.style.flex = `0 0 ${newRightWidth}px`;
				e.preventDefault();
				let visible = newRightWidth > 0 ? 1 : 0;
				if (visible !== lastVisible) {
					lastVisible = visible;
					sai.send(JSON.stringify({ schema: "com.warmcat.sai.builder_visibility", visible: visible }));
				}
			}
		};

		const onTouchEnd = () => {
			document.removeEventListener('touchmove', onTouchMove);
			document.removeEventListener('touchend', onTouchEnd);
			localStorage.setItem('sai-right-pane-flex', rightPane.style.flex);
		};

		const onTouchStart = (e) => {
			if (e.touches.length === 1) {
				x = e.touches[0].clientX;
				rightWidth = rightPane.getBoundingClientRect().width;
				lastVisible = rightWidth > 0 ? 1 : 0;
				document.addEventListener('touchmove', onTouchMove, { passive: false });
				document.addEventListener('touchend', onTouchEnd);
			}
		};

		resizer.addEventListener('mousedown', onMouseDown);
		resizer.addEventListener('touchstart', onTouchStart);
	}

	const savedTasksHeight = localStorage.getItem('sai-tasks-height');
	if (savedTasksHeight) {
		const tasksSection = document.getElementById('sai_event_tasks');
		if (tasksSection) {
			tasksSection.style.flex = savedTasksHeight;
		}
	}

	const resizerH = document.getElementById('resizer_h');
	if (resizerH) {
		const tasksSection = resizerH.previousElementSibling;

		let y = 0;
		let tasksHeight = 0;

		const onMouseMoveH = (e) => {
			const dy = e.clientY - y;
			const newHeight = tasksHeight + dy;
			tasksSection.style.flex = `0 0 ${newHeight}px`;
		};

		const onMouseUpH = () => {
			document.removeEventListener('mousemove', onMouseMoveH);
			document.removeEventListener('mouseup', onMouseUpH);
			localStorage.setItem('sai-tasks-height', tasksSection.style.flex);
		};

		const onMouseDownH = (e) => {
			y = e.clientY;
			tasksHeight = tasksSection.getBoundingClientRect().height;
			document.addEventListener('mousemove', onMouseMoveH);
			document.addEventListener('mouseup', onMouseUpH);
		};

		const onTouchMoveH = (e) => {
			if (e.touches.length === 1) {
				const dy = e.touches[0].clientY - y;
				const newHeight = tasksHeight + dy;
				tasksSection.style.flex = `0 0 ${newHeight}px`;
				e.preventDefault();
			}
		};

		const onTouchEndH = () => {
			document.removeEventListener('touchmove', onTouchMoveH);
			document.removeEventListener('touchend', onTouchEndH);
			localStorage.setItem('sai-tasks-height', tasksSection.style.flex);
		};

		const onTouchStartH = (e) => {
			if (e.touches.length === 1) {
				y = e.touches[0].clientY;
				tasksHeight = tasksSection.getBoundingClientRect().height;
				document.addEventListener('touchmove', onTouchMoveH, { passive: false });
				document.addEventListener('touchend', onTouchEndH);
			}
		};

		resizerH.addEventListener('mousedown', onMouseDownH);
		resizerH.addEventListener('touchstart', onTouchStartH);
	}
}, false);

}());
