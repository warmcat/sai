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
 	"\"Created %{pf} ago, creation time: %{ct}ms \":" +
 	   "\"%{pf}間前に作成されました, 作成にかかった時間: %{ct}ms\"" +
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
  "\"Created %{pf} ago, creation time: %{ct}ms \":" +
  	"\"%{pf}前創建, 創作時間: %{ct}ms \"" +
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

var logs = "", redpend = 0, gitohashi_integ = 0, authd = 0, exptimer, auth_user = "",
	ongoing_task_activities = {}, last_log_timestamp = 0;

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
	if (s.search("<") !== -1)
		return "invalid string";
	
	return s;
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
	}).replace(/\n/g, '<br>');
}

var pos = 0, lli = 1, lines = "", times = "", locked = 1, tfirst = 0,
		cont = [ 0, 0, 0, 0, 0];

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

function aging()
{
        var n, next = 24 * 3600,
            now_ut = Math.round((new Date().getTime() / 1000));

        for (n = 0; n < age_names.length; n++) {
                var m, elems = document.getElementsByClassName(
                				"age-" + n), ne = elems.length,
                				a = new Array(), ee = new Array();

                if (elems.length && age_upd[n] < next)
                        next = age_upd[n];

                for (m = 0; m < ne; m++) {
                        var e = elems[m], secs = elems[m].getAttribute("ut");

                        a.push(agify(now_ut, secs));
                        ee.push(e);
                }
                
                for (m = 0; m < ee.length; m++) {
                	ee[m].innerHTML = a[m]; 
                }
        }

	if (next < 5)
		next = 5;

        /*
         * We only need to come back when the age might have changed.
         * Eg, if everything is counted in hours already, once per
         * 5 minutes is accurate enough.
         */
        window.setTimeout(aging, next * 1000);
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
		san(t.t.platform) + "</span>&nbsp;";
	if (authd && t.t.state != 0 && t.t.state != 3 && t.t.state != 4 && t.t.state != 5)
		s += "<img class=\"rebuild\" alt=\"stop build\" src=\"stop.svg\" " +
			"id=\"stop-" + san(t.t.uuid) + "\">&nbsp;";
	if (authd)
		s += "<img class=\"rebuild\" alt=\"rebuild\" src=\"rebuild.png\" " +
			"id=\"rebuild-" + san(t.t.uuid) + "\">&nbsp;" +
			sai_stateful_taskname(t.t.state, t.t.taskname, 1);
		
	if (t.t.builder_name) {
		var now_ut = Math.round((new Date().getTime() / 1000));

		s += "&nbsp;&nbsp;<span class=\"ti5\"><img class=\"bico\" src=\"/sai/builder-instance.png\">&nbsp;" +
			san(t.t.builder_name) + "</span>";
		if (t.t.started)
		/* started is a unix time, in seconds */
		s += ", <span class=\"ti5\"> " +
		     agify(now_ut, t.t.started) + " ago, Dur: " +
		     (t.t.duration ? t.t.duration / 1000000 :
		     	now_ut - t.t.started).toFixed(1) +
		     	"s</span><div id=\"sai_arts\"></div>";
		sai_arts = "";
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

    if (summary.total > 0) {
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
	var good = 0, bad = 0, total = 0, ongoing = 0, pending = 0,
		roo = document.getElementById("taskcont-" + event_uuid),
		same;
		
	if (!roo) 
		return { text: "" };
	
	same = roo.querySelectorAll(".taskstate");
	if (same)
		total = same.length;
	same = roo.querySelectorAll(".taskstate0");
	if (same)
		pending = same.length;
	same = roo.querySelectorAll(".taskstate1");
	if (same)
		ongoing += same.length;
	same = roo.querySelectorAll(".taskstate2");
	if (same)
		ongoing += same.length;
	same = roo.querySelectorAll(".taskstate3");
	if (same)
		good = same.length;
	same = roo.querySelectorAll(".taskstate4");
	if (same)
		bad += same.length;
	same = roo.querySelectorAll(".taskstate5");
	if (same)
		bad += same.length; // treat cancelled as bad
	same = roo.querySelectorAll(".taskstate6");
	if (same)
		ongoing += same.length;

	var text;
	if (good == total && total > 0)
		text = "All " + good + " passed";
	else if (bad == total && total > 0)
		text = "All " + bad + " failed";
	else if (pending == total && total > 0)
		text = total + " pending";
	else {
		var parts = [];
		if (good) parts.push(good + " passed");
		if (bad) parts.push(bad + " failed");
		if (ongoing) parts.push(ongoing + " ongoing");
		if (pending) parts.push(pending + " pending");
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
	if (reset_all_icon && !gitohashi_integ && authd) {
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
		"<span class=\"e1\">" + san(e.repo_name) +
		"</span></td></tr><tr><td class=\"nomar\" colspan=2><span class=\"e2\">";
	
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
		        san(e.hash.substr(0, 8)) +
		     "</td><td class=\"e6 nomar\">" +
		     agify(now_ut, e.created) + "</td></tr>";
		 s += "</table>" +
		     "</td>";
	} else {
		s +="<td><table><tr><td class=\"e6 nomar\">" + san(e.hash.substr(0, 8)) + " " + agify(now_ut, e.created) +
		     "</td></tr><tr><td class=\"nomar e6\" id=\"sumbs-" + e.uuid + "\"></td></tr>" +
		     "</table></td>";
	}
	s += "</tr><tr><td class=\"nomar e6\" colspan=\"2\" id=\"sumbs-" + e.uuid +"\"></td></tr></table>";
	     
	return s;
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

		for (q = 0; q < o.t.length; q++) {
			var t = o.t[q];
			
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
				"\" data-event-uuid=\"" + san(e.uuid) + "\" data-platform=\"" + san(t.platform) + "\">";
			s1 += "<a href=\"/sai/index.html?task=" + t.uuid + "\">" +
				sai_plat_icon(t.platform, 0) + "</a>";
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
	
	return s;
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

function refresh_state(task_uuid, task_state)
{
	var tsi = document.getElementById("taskstate_" + task_uuid);
	
	if (tsi) {
		tsi.classList.remove("taskstate1");
		tsi.classList.remove("taskstate2");
		tsi.classList.remove("taskstate3");
		tsi.classList.remove("taskstate4");
		tsi.classList.remove("taskstate5");
		tsi.classList.remove("taskstate6");
		tsi.classList.remove("taskstate7");
		tsi.classList.add("taskstate" + task_state);
		// console.log("refresh_state  taskstate" + task_state);
	}
}

function after_delete() {
	location.reload();
}

function createContextMenu(event, menuItems) {
    event.preventDefault();

    // Remove any existing context menu
    const existingMenus = document.querySelectorAll(".context-menu");
    existingMenus.forEach(menu => {
        document.body.removeChild(menu);
    });

    const menu = document.createElement("div");
    menu.className = "context-menu";
    menu.style.top = event.pageY + "px";
    menu.style.left = event.pageX + "px";

    const ul = document.createElement("ul");
    menu.appendChild(ul);

    menuItems.forEach(item => {
        const li = document.createElement("li");
        li.innerHTML = item.label;
        if (item.callback) {
            li.addEventListener("click", item.callback);
        }
        ul.appendChild(li);
    });

    document.body.appendChild(menu);

    const closeMenu = () => {
        if (document.body.contains(menu)) {
            document.body.removeChild(menu);
        }
        document.removeEventListener("click", closeMenu);
    };

    setTimeout(() => {
        document.addEventListener("click", closeMenu);
    }, 0);
}

function createBuilderDiv(plat) {
	const platDiv = document.createElement("div");
	platDiv.className = "ibuil bdr";
	if (!plat.online)
		platDiv.className += " offline";
	// Add a new class if the builder is powering up
	if (plat.powering_up)
		platDiv.className += " powering-up";
	if (plat.powering_down)
		platDiv.className += " powering-down";

	platDiv.id = "binfo-" + plat.name;
	platDiv.title = plat.platform + "@" + plat.name.split('.')[0] + " / " + plat.peer_ip;

	let plat_parts = plat.platform.split('/');
	let plat_os = plat_parts[0] || 'generic';
	let plat_arch = plat_parts[1] || 'generic';
	let plat_tc = plat_parts[2] || 'generic';

	let innerHTML = `<table class="nomar"><tbody><tr><td class="bn">`;
	innerHTML += `<img class="ip1 zup" src="/sai/${plat_os}.svg" onerror="this.src='/sai/generic.svg';this.onerror=null;">`;
	innerHTML += `<img class="ip1 tread1" src="/sai/arch-${plat_arch}.svg" onerror="this.src='/sai/generic.svg';this.onerror=null;">`;
	innerHTML += `<img class="ip1 tread2" src="/sai/tc-${plat_tc}.svg" onerror="this.src='/sai/generic.svg';this.onerror=null;">`;
	innerHTML += `<br>${plat.peer_ip}`;
	innerHTML += `<div class="instload" id="instload-${plat.name}">`;

	// Create initial idle squares
	for (let i = 0; i < plat.instances; i++) {
		innerHTML += `<div class="inst_box inst_idle" title="instance ${i}: idle">` +
		             `<div class="inst_bar"></div>` +
		             `</div>`;
	}

	innerHTML += `</div></td></tr></tbody></table>`;

	platDiv.innerHTML = innerHTML;

	const menuItems = [
		{ label: `<b>SAI Hash:</b> ${plat.sai_hash}` },
		{ label: `<b>LWS Hash:</b> ${plat.lws_hash}` },
		{
			label: "Update SAI",
			callback: () => {
				const rebuildMsg = {
					schema: "com.warmcat.sai.rebuild",
					builder_name: plat.name
				};
				sai.send(JSON.stringify(rebuildMsg));
			}
		}
	];

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
//	if (s1.split("?"))
//	s1 = s1.split("?")[0];
	console.log(s1);
	sai = new WebSocket(s1, "com-warmcat-sai");

	try {
		sai.onopen = function() {
			var par = new URLSearchParams(window.location.search),
				tid, eid;
			tid = par.get('task');
			eid = par.get('event');
		
			
			if (tid) {
				/*
				 * We're being the page monitoring / reporting
				 * on what happened with a specific task... ask
				 * about the specific task on the ws link
				 */
				 
				 console.log("tid " + tid);
				 
				 sai.send("{\"schema\":" +
				 	  "\"com.warmcat.sai.taskinfo\"," +
					  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
				 	  "\"logs\": 1," +
					  "\"last_log_ts\":" + last_log_timestamp + "," +
				 	  "\"task_hash\":" +
				 	  JSON.stringify(tid) + "}");
				 	  
				 return;
			}
			
			if (eid) {
				/*
				 * We're being the page monitoring / reporting
				 * on what happened with a specific event... ask
				 * about the specific event on the ws link
				 */
				 
				 console.log("eid " + eid);
				 
				 sai.send("{\"schema\":" +
				 	  "\"com.warmcat.sai.eventinfo\"," +
					  "\"js_api_version\": " + SAI_JS_API_VERSION + "," +
					  "\"event_hash\":" +
				 	  JSON.stringify(eid) + "}");
				 	  
				 return;
			}
			
			/*
			 * request the overview schema
			 */
			
			 sai.send("{\"schema\":" +
			 	  "\"com.warmcat.sai.taskinfo\", \"js_api_version\": " + SAI_JS_API_VERSION + "}");
		};

		sai.onmessage = function got_packet(msg) {
			var u, ci, n;
			var now_ut = Math.round((new Date().getTime() / 1000));

		//	console.log(msg.data);
		//	if (msg.data.length < 10)
		//		return;
			jso = JSON.parse(msg.data);
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

			switch (jso.schema) {                 

 			case "com.warmcat.sai.builders":
				const buildersContainer = document.getElementById("sai_builders");
				if (!buildersContainer) { break; }

				let platformsArray = null;
				if (jso.platforms && Array.isArray(jso.platforms)) {
					platformsArray = jso.platforms;
				} else if (jso.builders && Array.isArray(jso.builders)) {
					platformsArray = jso.builders;
				}
				if (!platformsArray) {
					buildersContainer.innerHTML = ""; // Clear display if data is invalid
					break;
				}

				// --- Reconciliation Logic ---

				// Step 1: Find the main content area (the TD). Create it if this is the first run.
				let tdContainer = buildersContainer.querySelector("table td");
				if (!tdContainer) {
					buildersContainer.innerHTML = ""; // Clear for safety
					const table = document.createElement("table");
					table.className = "builders";
					const tbody = document.createElement("tbody");
					const tr = document.createElement("tr");
					tdContainer = document.createElement("td");
					tr.appendChild(tdContainer);
					tbody.appendChild(tr);
					table.appendChild(tbody);
					buildersContainer.appendChild(table);
				}

				// Step 2: Detach all existing builder divs and store them in a map for reuse.
				// This preserves them so their CSS transitions will work.
				const existingDivs = new Map();
				tdContainer.querySelectorAll(".bdr").forEach(div => {
					const name = div.id.substring(6); // "binfo-" is 6 chars
					if (name) {
						existingDivs.set(name, div);
					}
				});
				tdContainer.innerHTML = ""; // Clear the container, but the divs are still in memory.

				// Step 3: Rebuild the group structure from scratch, reusing the old divs.
				platformsArray.sort((a, b) => {
					if (a.name && b.name) {
						return a.name.localeCompare(b.name);
					}
					return 0; // Don't sort if names are missing
				});

				const platformsByGroup = {};
				for (const plat of platformsArray) {
					const groupKey = getBuilderGroupKey(plat.name);
					if (!platformsByGroup[groupKey]) platformsByGroup[groupKey] = [];
					platformsByGroup[groupKey].push(plat);
				}

               const groupKeys = Object.keys(platformsByGroup).sort();
               for (const key of groupKeys) {
                       const groupPlatforms = platformsByGroup[key];
                       const nestedPlatforms = groupPlatforms.filter(p => getBuilderHostname(p.name) !== key);
                       const mainPlatforms = groupPlatforms.filter(p => getBuilderHostname(p.name) === key);

                       if (nestedPlatforms.length > 0) {
                               const groupDiv = document.createElement("div");
                               groupDiv.className = "ibuil ibuilctr bdr";
                               
                               const nameDiv = document.createElement("div");
                               nameDiv.className = "ibuilctrname bdr";
                               nameDiv.textContent = key;
                               groupDiv.appendChild(nameDiv);

                               nestedPlatforms.forEach(plat => {
                                       const div = existingDivs.get(plat.name) || createBuilderDiv(plat);
                                       div.classList.toggle('offline', !plat.online);
                                       div.classList.toggle('powering-up', !!plat.powering_up);
                                       div.classList.toggle('powering-down', !!plat.powering_down);
                                       groupDiv.appendChild(div);
                               });
                               tdContainer.appendChild(groupDiv);
                       }

                       mainPlatforms.forEach(plat => {
                               const div = existingDivs.get(plat.name) || createBuilderDiv(plat);
                               div.classList.toggle('offline', !plat.online);
                               div.classList.toggle('powering-up', !!plat.powering_up);
                               div.classList.toggle('powering-down', !!plat.powering_down);
                               tdContainer.appendChild(div);
                       });
               }
 				break;

			case "sai.warmcat.com.overview":
				/*
				 * Sent with an array of e[] to start, but also
				 * can send a single e[] if it just changed
				 * state
				 */ 
				s = "<table>";
				
				authd = jso.authorized;
				if (jso.authorized === 0) {
					if (document.getElementById("creds"))
						document.getElementById("creds").classList.remove("hide");
					if (document.getElementById("logout"))
						document.getElementById("logout").classList.add("hide");
				}
				if (jso.authorized === 1) {
					if (document.getElementById("creds"))
						document.getElementById("creds").classList.add("hide");
					if (document.getElementById("logout"))
						document.getElementById("logout").classList.remove("hide");
					if (jso.auth_user)
						auth_user = jso.auth_user;
					if (jso.auth_secs) {
						var now_ut = Math.round((new Date().getTime() / 1000));
						clearTimeout(exptimer);
						exptimer = window.setTimeout(expiry, 1000 * jso.auth_secs);
						if (document.getElementById("remauth"))
							document.getElementById("remauth").innerHTML =
								san(auth_user) + " " + agify(now_ut, now_ut + jso.auth_secs);
					}
				}
				
				/*
				 * Update existing?
				 */
			
				// console.log("jso.overview.length " + jso.overview.length);
				
				if (jso.overview.length == 1 &&
				    document.getElementById("esr-" + jso.overview[0].e.uuid)) {
					/* this is just the summary box, not the tasks */
					document.getElementById("esr-" + jso.overview[0].e.uuid).innerHTML =
						sai_event_summary_render(jso.overview[0], now_ut, 1);
						
					/* if the task status icons exist, update their state */
						
					for (n = jso.overview[0].t.length - 1; n >= 0; n--)
						refresh_state(jso.overview[0].t[n].uuid, jso.overview[0].t[n].state);
					
					update_summary_and_progress(jso.overview[0].e.uuid);
						
					aging();
				} else
				{
					/*
					 * display events wholesale
					 */
					if (jso.overview.length) {
						for (n = jso.overview.length - 1; n >= 0; n--)
							s += sai_event_render(jso.overview[n], now_ut, 1);
				
						s = s + "</table>";
					
						if (document.getElementById("sai_sticky"))
							document.getElementById("sai_sticky").innerHTML = s;
						
						for (n = jso.overview.length - 1; n >= 0; n--) {
							document.getElementById("esr-" + jso.overview[n].e.uuid).innerHTML =
								sai_event_summary_render(jso.overview[n], now_ut, 1);

							update_summary_and_progress(jso.overview[n].e.uuid);
						}
						aging();
					}
						
					if (gitohashi_integ && document.getElementById("gitohashi_sai_icon")) {
						var integ_state = 0;
						document.getElementById("gitohashi_sai_icon").addEventListener("mouseenter", function( event ) {
							document.getElementById("gitohashi_sai_icon").style.zIndex = 1999;  
							document.getElementById("gitohashi_sai_details").style.zIndex = 2000;
							document.getElementById("gitohashi_sai_details").style.opacity = 1.0; 
							integ_state = 1;
						}, false);

						document.getElementById("gitohashi_sai_details").addEventListener("mouseout", function( event ) {
							var e = event.toElement || event.relatedTarget;
							while (e && e.parentNode && e.parentNode != window) {
							    if (e.parentNode == this ||  e == this) {
							        if (e.preventDefault)
									e.preventDefault();
							        return false;
							    }
							    e = e.parentNode;
							}
							document.getElementById("gitohashi_sai_details").style.opacity = 0.0;
							document.getElementById("gitohashi_sai_details").style.zIndex = -1;
						 	document.getElementById("gitohashi_sai_icon").style.zIndex = 2001;
						}, true);
						
						aging();
					}
				}
				
				if (jso.overview.length)
					for (n = jso.overview.length - 1; n >= 0; n--) {
						if (document.getElementById("rebuild-ev-" + san(jso.overview[n].e.uuid)))
							document.getElementById("rebuild-ev-" + san(jso.overview[n].e.uuid)).
								addEventListener("click", function(e) {
					console.log(e);
						var rs= "{\"schema\":" +
				 	 	 "\"com.warmcat.sai.eventreset\"," +
				 	  	 "\"uuid\": " +
				 	  	 	JSON.stringify(san(e.srcElement.id.substring(11))) + "}";
				 	  	 	
				 	  	console.log(rs);
				 	  	sai.send(rs);
					});
					if (document.getElementById("delete-ev-" + san(jso.overview[n].e.uuid)))
						document.getElementById("delete-ev-" + san(jso.overview[n].e.uuid)).
							addEventListener("click", function(e) {
					console.log(e);
						var rs= "{\"schema\":" +
				 	 	 "\"com.warmcat.sai.eventdelete\"," +
				 	  	 "\"uuid\": " +
				 	  	 	JSON.stringify(san(e.srcElement.id.substring(10))) + "}";
				 	  	 	
				 	  	console.log(rs);
				 	  	sai.send(rs);
						setTimeout(after_delete, 750);
					});
				}
				break;
			
			case "com.warmcat.sai.taskinfo":

				if (!jso.t)
					break;

				authd = jso.authorized;
				if (jso.authorized === 0) {
					if (document.getElementById("creds"))
						document.getElementById("creds").classList.remove("hide");
					if (document.getElementById("logout"))
						document.getElementById("logout").classList.add("hide");
				}
				if (jso.authorized === 1) {
					if (document.getElementById("creds"))
						document.getElementById("creds").classList.add("hide");
					if (document.getElementById("logout"))
						document.getElementById("logout").classList.remove("hide");
					if (jso.auth_user)
						auth_user = jso.auth_user;
					if (jso.auth_secs) {
						var now_ut = Math.round((new Date().getTime() / 1000));
						clearTimeout(exptimer);
						exptimer = window.setTimeout(expiry, 1000 * jso.auth_secs);
						if (document.getElementById("remauth"))
							document.getElementById("remauth").innerHTML =
								san(auth_user) + " " + agify(now_ut, now_ut + jso.auth_secs);
					}
				}
				
				/*
				 * We get told about changes to any task state,
				 * it's up to us to figure out if the page we
				 * showed should display the update and in what
				 * form.
				 *
				 * We make sure the div containing the task info
				 * has a special ID depending on if it's shown
				 * as a tuple or as extended info
				 *
				 * First see if it appears as a tuple, and if
				 * so, let's just update that
				 */
				
				if (document.getElementById("taskstate_" + jso.t.uuid)) {
					console.log("found taskstate_" + jso.t.uuid);
					refresh_state(jso.t.uuid, jso.t.state);
					
					update_summary_and_progress(jso.t.uuid.substring(0, 32));

				} else
				
					/* update task summary if shown anywhere */
					
					if (document.getElementById("taskinfo-" + jso.t.uuid)) {
						console.log("FOUND taskinfo-" + jso.t.uuid);
						document.getElementById("taskinfo-" + jso.t.uuid).innerHTML = sai_taskinfo_render(jso);
						if (document.getElementById("esr-" + jso.e.uuid))
							document.getElementById("esr-" + jso.e.uuid).innerHTML =
								sai_event_summary_render(jso, now_ut, 1);
					} else {
						
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
						    document.getElementById("sai_sticky"))
							document.getElementById("sai_sticky").innerHTML =
								"<div class=\"taskinfo\" id=\"taskinfo-" +
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
					
						if (document.getElementById("esr-" + jso.e.uuid))
							document.getElementById("esr-" + jso.e.uuid).innerHTML =
								sai_event_summary_render(jso, now_ut, 1);
								
						update_summary_and_progress(jso.e.uuid);
					}
	
					if (document.getElementById("rebuild-" + san(jso.t.uuid))) {
						document.getElementById("rebuild-" + san(jso.t.uuid)).
							addEventListener("click", function(e) {
								var rs= "{\"schema\":" +
						 	 	 "\"com.warmcat.sai.taskreset\"," +
						 	  	 "\"uuid\": " +
						 	  	 	JSON.stringify(san(e.srcElement.id.substring(8))) + "}";
						 	  	 	
						 	  	console.log(rs);
						 	  	sai.send(rs);
						 	  	document.getElementById("dlogsn").innerHTML = "";
						 	  	document.getElementById("dlogst").innerHTML = "";
						 	  	document.getElementById("logs").innerHTML = "";
						 	  	lines = times = logs = "";
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
				}
				break;

	case "com.warmcat.sai.loadreport":
		if (!jso.platforms || !Array.isArray(jso.platforms)) {
			break;
		}

		for (const platformReport of jso.platforms) {
			const platformName = platformReport.platform_name;
			if (!platformName) {
				continue;
			}
			
			const loadContainer = document.getElementById("instload-" + platformName);
			if (!loadContainer) {
				continue;
			}

			if (platformReport.loads && Array.isArray(platformReport.loads)) {
				const instanceDivs = loadContainer.getElementsByClassName("inst_box");
				
				for (let i = 0; i < platformReport.loads.length; i++) {
					const instanceLoad = platformReport.loads[i];
					const instanceDiv = instanceDivs[i];

					if (!instanceDiv) {
						break;
					}

					let cpu = instanceLoad.cpu_percent / 10.0;
					let stateText = instanceLoad.state ? 'busy' : 'idle';
					instanceDiv.title = `Instance ${i}: ${stateText}\nCPU: ${cpu.toFixed(1)}%`;

					if (instanceLoad.state) {
						instanceDiv.classList.add("inst_busy");
						instanceDiv.classList.remove("inst_idle");
					} else {
						instanceDiv.classList.add("inst_idle");
						instanceDiv.classList.remove("inst_busy");
					}

					// The bar is always the first (and only) child of the inst_box div.
					const bar = instanceDiv.firstChild;
					if (bar && bar.classList.contains("inst_bar")) {
						// cpu_percent is in tenths of a percent, relative to ONE core.
						// So 1000 = 100% = 1 full core.
						let total_cpu_capacity = jso.core_count * 1000;
						
						// Normalize the load to be a percentage of the ENTIRE system's capacity
						let cpu_percentage = (instanceLoad.cpu_percent / total_cpu_capacity) * 100;

						if (cpu_percentage > 100) cpu_percentage = 100;
						if (cpu_percentage < 0) cpu_percentage = 0;
						if (cpu_percentage > 0 && cpu_percentage < 1) cpu_percentage = 1;

						bar.style.height = `${cpu_percentage}%`;

						// Update the class for idle/busy state
						if (instanceLoad.state) { // state == 1 means busy
						    instanceDiv.classList.add("inst_busy");
						    instanceDiv.classList.remove("inst_idle");
						} else { // state == 0 means idle
						    instanceDiv.classList.add("inst_idle");
						    instanceDiv.classList.remove("inst_busy");
						}
					} else {
						// This console log will tell us if the bar element is missing
						console.error("Could not find .inst_bar child in .inst_box for", instanceDiv);
					}
				}
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
				}
				break;

			case "com.warmcat.sai.unauthorized":
				location.reload();
				break;

			case "com-warmcat-sai-logs":
				var s1 = atob(jso.log), s = hsanitize(s1), li,
					en = "", yo, dh, ce, tn = "";
					
				if (!tfirst)
					tfirst = jso.timestamp;

				last_log_timestamp = jso.timestamp;
				
				li = (s1.match(/\n/g)||[]).length;
				
				switch (jso.channel) {
				case 1:
					logs += s;
					break;
				case 2:
					logs += "<span class=\"stderr\">" + s +
							"</span>";
					break;
				case 3:
					logs += "<span class=\"saibuild\">\u{25a0} " + s +
							"</span>";
					break;
				case 4:
					logs += "<span class=\"tty0\">" + s +
							"</span>";
					break;
				default:
					logs += "<span class=\"tty1\">" + s +
							"</span>";


				}
				
				if (cont && !cont[jso.channel] && jso.len)
					tn = ((jso.timestamp - tfirst) / 1000000).toFixed(4);

				if (cont)
				cont[jso.channel] = (li == 0);
					
				while (li--) {
					en += "<a id=\"#sn" + lli +
						"\" href=\"#sn" + lli + "\">" +
						lli + "</a><br>";
					tn += "<br>"
					lli++;
				}
					
				lines += en;
				times += tn;
				
				if (!redpend) {
					redpend = 1;
					setTimeout(function() {
		redpend = 0;
		locked = document.body.scrollHeight -
			document.body.clientHeight <=
			document.body.scrollTop + 1;

		if (document.getElementById("logs")) {
			document.getElementById("logs").innerHTML = logs;

			if (document.getElementById("dlogsn"))
				document.getElementById("dlogsn").innerHTML = lines;
				
			if (document.getElementById("dlogst"))
				document.getElementById("dlogst").innerHTML = times;
		}

		if (locked)
		   document.body.scrollTop =
			document.body.scrollHeight -
			document.body.clientHeight;
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
			myVar = setTimeout(ws_open_sai, 4000);
		};
	} catch(exception) {
		alert("<p>Error" + exception);  
	}
}

function post_login_form()
{
	var xhr = new XMLHttpRequest(), s ="", q = window.location.pathname;
	
	s = "----boundo\x0d\x0acontent-disposition: form-data; name=\"lname\"\x0d\x0a\x0d\x0a" +
		document.getElementById("lname").value +
	    "\x0d\x0a----boundo\x0d\x0acontent-disposition: form-data; name=\"lpass\"\x0d\x0a\x0d\x0a" +
		document.getElementById("lpass").value + 
	    "\x0d\x0a----boundo\x0d\x0acontent-disposition: form-data; name=\"success_redir\"\x0d\x0a\x0d\x0a" +
		document.getElementById("success_redir").value +
	    "\x0d\x0a----boundo--";

	if (q.length > 10 && q.substring(q.length - 10) == "index.html")
		q = q.substring(0, q.length - 10);
	xhr.open("POST", q + "login", true);
	xhr.setRequestHeader( 'content-type', "multipart/form-data; boundary=--boundo");
	
	console.log(s.length +" " + s);

	xhr.onload = function (e) {
	  if (xhr.readyState === 4) {
	    if (xhr.status === 200 || xhr.status == 303) {
	      console.log(xhr.responseText);
		location.reload();
	    } else {
	      console.error(xhr.statusText);
	    }
	  }
	};
	xhr.onerror = function (e) {
	  console.error(xhr.statusText);
	};

	xhr.send(s);
	
	return false;
}

/* stuff that has to be delayed until all the page assets are loaded */

window.addEventListener("load", function() {
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
		
	/* login form hidden success redirect */
	if (document.getElementById("success_redir"))
		document.getElementById("success_redir").value =
			window.location.href;
	ws_open_sai();
	aging();

	if (document.getElementById("login")) {
		document.getElementById("login").addEventListener("click", post_login_form);
		document.getElementById("logout").addEventListener("click", post_login_form);
	}
	
	setInterval(function() {
		update_task_activities();

	    var locked = document.body.scrollHeight -
	    	document.body.clientHeight <= document.body.scrollTop + 1;
	
	    if (locked)
	     document.body.scrollTop = document.body.scrollHeight -
	     	document.body.clientHeight;

	}, 500)

	const stickyEl = document.getElementById("sai_sticky");
	if (stickyEl) {
		stickyEl.addEventListener("contextmenu", function(event) {
			let target = event.target;
			let taskDiv = null;

			// find the taskstate div parent
			while (target && target.id !== "sai_sticky") {
				if (target.classList && target.classList.contains("taskstate")) {
					taskDiv = target;
					break;
				}
				target = target.parentElement;
			}

			if (taskDiv && authd) {
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

				createContextMenu(event, menuItems);
			}
		});
	}
	
}, false);

}());
