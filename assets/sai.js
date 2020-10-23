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

var logs = "", redpend = 0, gitohashi_integ = 0, authd = 0, exptimer, auth_user = "";
	
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
	console.log("plat " + plat + " plat[0] " + s[0]);
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
	
	s = "<div class=\"taskinfo\"><table><tr class=\"nomar\"><td class=\"atop\"><table>" +
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
	
	s += "</td></tr></table></table></div>";
	
	return s;
}

function sai_event_summary_render(o, now_ut, reset_all_icon)
{
	var s, q, ctn = "", wai, s1 = "", n, e = o.e;

	s = "<tr><td class=\"waiting\"><table class=\"comp";
	if (e.state == 3)
		s += " comp_pass";
	if (e.state == 4 || e.state == 6)
		s += " comp_fail";

	s += "\"><tr><td class=\"jumble\"><a href=\"/sai/?event=" + san(e.uuid) +
		"\"><img src=\"/sai/sai-event.svg\"";
	if (e.state == 3 || e.state == 4)
		s += " class=\"deemph\"";
	s += ">";
	if (e.state == 3)
		s += "<div class=\"evr\"><img src=\"/sai/passed.svg\"></div>";
	if (e.state == 4)
		s += "<div class=\"evr\"><img src=\"/sai/failed.svg\"></div>";
		
	s += "</a>";
	if (reset_all_icon && !gitohashi_integ && authd) {
		s += "<br><img class=\"rebuild\" alt=\"rebuild all\" src=\"/sai/rebuild.png\" " +
			"id=\"rebuild-ev-" + san(e.uuid) + "\">&nbsp;";
		s += "<img class=\"rebuild\" alt=\"delete event\" src=\"/sai/delete.png\" " +
				"id=\"delete-ev-" + san(e.uuid) + "\">";
	}
	s += "</td>" +
		"<td><table class=\"nomar\">" +
		"<tr><td class=\"nomar\" colspan=2><span class=\"e1\">" +
		san(e.repo_name) +
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

	s += "</span></td></tr><tr><td class=\"nomar\"><span class=\"e3\">" +
	        san(e.hash.substr(0, 8)) +
	     "</span></td><td class=\"e4\" class=\"nomar\">" +
	     agify(now_ut, e.created) + "</td></tr>";
	 if (o.t.length > 1)
	     	s += "<tr><td class=\"nomar\"><span class=\"e3\">" + o.t.length + " builds</span></td><tr>";
	 s += "</table>" +
	     "</td></tr></table></td>";
	     
	return s;
}

function sai_event_render(o, now_ut, reset_all_icon)
{
	var s, q, ctn = "", wai, s1 = "", n, e = o.e;

	s = sai_event_summary_render(o, now_ut, reset_all_icon);

	if (o.t.length) {
		var all_good, has_bad, all_done;
		
		/*
		 * SAIES_WAITING,
		 * SAIES_PASSED_TO_BUILDER,
		 * SAIES_BEING_BUILT,
		 * SAIES_SUCCESS, done --> 
		 * SAIES_FAIL,
		 * SAIES_CANCELLED,
		 * SAIES_BEING_BUILT_HAS_FAILURES
		 */
		
		s += "<td class=\"tasks\"><table><tr><td class=\"atop\">";

		for (q = 0; q < o.t.length; q++) {
			var t = o.t[q];
			
			if (t.taskname !== ctn) {
				if (ctn !== "") {
					var st = 0;
					/*
					 * We defer issuing the task overview
					 * until we know the class element to
					 * use for the container
					 */

					s += "<div class=\"";
					if (wai)
						s += " awaiting";
					if (all_good === 1) {
						s += " ov_good";
						st = 3;
					} else {
						if (has_bad === 1) {
							s += " ov_bad";
							st = 4;
						} else
							if (all_done === 0)
								s += " ov_dunno";
					}

					s += " ib\"><table class=\"nomar\">" +
					     "<tr><td class=\"keepline\">" + s1;
					s += "</td><td class=\"tn\">" +
						sai_stateful_taskname(st, ctn, 0) +
						"</td></tr></table></div>";
					s1 = "";
				}
				all_good = 1;
				has_bad = 0;
				all_done = 1;
				ctn = t.taskname;
				wai = 0;
			}
			
			if (t.state < 3)
				all_done = 0;
			if (t.state !== 3)
				all_good = 0;
			if (t.state === 4 || t.state == 6)
				has_bad = 1;

			s1 += "<div class=\"taskstate taskstate" + t.state + "\">";
			if (t.state === 0)
				wai = 1;

			s1 += "<a href=\"/sai/index.html?task=" + t.uuid + "\">" +
				sai_plat_icon(t.platform, 0) + "</a>";

			s1 += "</div>";
		}

		if (ctn !== "") {
			var st = 0;

			s += "<div class=\"";
			if (wai)
				s += " awaiting";
			if (all_good === 1) {
				s += " ov_good";
				st = 3;
			} else {
				if (has_bad === 1) {
					s += " ov_bad";
					st = 4;
				} else
					if (all_done === 0)
						s += " ov_dunno";
			}
			s += " ib\"><table class=\"nomar\">" +
				"<tr><td class=\"keepline\">" + s1;
			s += "</td><td class=\"tn\">" +
				sai_stateful_taskname(st, ctn, 0) +
			     		"</td></tr></table></div>";
		}

		s += "</td></tr></table></td>";
	}

	s += "</tr>";
	
	return s;
}

function render_builders(jso)
{
	var s, n, conts = [], nc = 0;

	s = "<table class=\"builders\"><tr><td>";
		
//	s = "<table class=\"builders\"><tr><td><img class=\"ip bsvg\" " +
//		"src=\"/sai/builder.png\"></td><td>";
		
	for (n = 0; n < jso.builders.length; n++) {
		var e = jso.builders[n], host;

		host = e.name.split('.')[0];
		if (host.split('-')[1]) {
			var m;
			
			for (m = 0; m < conts.length; m++)
				if (host.split('-')[1] === conts[m])
					m = 999;
			if (m < 999 || !conts.length)
				conts.push(host.split('-')[1]);
		}
	}
	
	for (nc = 0; nc <= conts.length; nc++) {
		var samplat = "";
	
		/*
		 * nc == conts.length means those not
		 * in a container
		 */
		 
		if (nc < conts.length)
			s += "<div class=\"ibuil ibuilctr bdr\"><div class=\"ibuilctrname bdr\">" + conts[nc] + "</div>";
	
		for (n = 0; n < jso.builders.length; n++) {
			var e = jso.builders[n], nn, host, plat, cia, sen;
	
			sen = e.name;
			host = sen.split('.')[0];
			cia = host.split('-')[1];
			
			console.log("host " + host + ", cia " + cia);
			
			if ((nc == conts.length && !cia) ||
			    (cia && cia === conts[nc])) {
			    	var did = 0, arc;
	
				if (!n ||
				    host !== jso.builders[n - 1].name.split('.')[0] ||
				    e.platform != samplat) {
				s += "<div class=\"ibuil bdr\" title=\"" +
					san(e.platform) + "@" + san(host) +
					"\"><table class=\"nomar\"><tr><td class=\"bn\">" +
					sai_plat_icon(e.platform, 1);

				samplat = e.platform;
				did = 1;
				}
			
				if (n + 1 == jso.builders.length || did)
					s += "</td></tr></table></div>";
			}
		}
		if (nc < conts.length)
			s += "</div>";
	}
	
	s += "</td></tr></table></td></tr>";
	s = s + "</table>";
				
	return s;
}

function ws_open_sai()
{	
	var s = "", q, qa, qi, q5, q5s;
	
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
				 	  "\"logs\": 1," +
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
				 	  "\"event_hash\":" +
				 	  JSON.stringify(eid) + "}");
				 	  
				 return;
			}
			
			/*
			 * request the overview schema
			 */
			
			 sai.send("{\"schema\":" +
			 	  "\"com.warmcat.sai.taskinfo\"}");
		};

		sai.onmessage =function got_packet(msg) {
			var u, ci, n;
			var now_ut = Math.round((new Date().getTime() / 1000));

			console.log(msg.data);
		//	if (msg.data.length < 10)
		//		return;
			jso = JSON.parse(msg.data);
			console.log(jso.schema);
			
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

			if (jso.schema == "com.warmcat.sai.builders") {

				s = render_builders(jso);
				
				if (document.getElementById("sai_builders"))
				document.getElementById("sai_builders").innerHTML = s;
			}
			
			if (jso.schema == "sai.warmcat.com.overview") {
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
			
				if (jso.overview.length)
					for (n = jso.overview.length - 1; n >= 0; n--)
						s += sai_event_render(jso.overview[n], now_ut, 1);
				
				s = s + "</table>";
				
				if (document.getElementById("sai_sticky"))
					document.getElementById("sai_sticky").innerHTML = s;
				aging();
				
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
					});
				} 
			}
			
			if (jso.schema == "com.warmcat.sai.taskinfo") {
			
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
			
				s = sai_taskinfo_render(jso);
						if (document.getElementById("sai_sticky"))
				document.getElementById("sai_sticky").innerHTML = s;
				
				
		s = "<table><td colspan=\"3\"><pre><table><tr>" +
		"<td class=\"atop\">" +
		"<div id=\"dlogsn\" class=\"dlogsn\">" + lines + "</div></td>" +
		"<td class=\"atop\">" +
		"<div id=\"dlogst\" class=\"dlogst\">" + times + "</div></td>" +
	     "<td class=\"atop\"><div id=\"dlogs\" class=\"dlogs\">" +
	     "<span id=\"logs\" class=\"nowrap\">" + logs +
	     	"</span>"+
		"</div></td></tr></table></pre>";
				
				if (document.getElementById("sai_overview"))
				document.getElementById("sai_overview").innerHTML = s;

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
			
			
			if (jso.schema == "com-warmcat-sai-artifact") {
				console.log(jso);
				
				sai_arts += "<div class=\"sai_arts\"><img src=\"artifact.svg\">&nbsp;<a href=\"artifacts/" +
					san(jso.task_uuid) + "/" +
					san(jso.artifact_down_nonce) + "/" +
					san(jso.blob_filename) + "\">" +
					san(jso.blob_filename) + "</a>&nbsp;" +
					humanize(jso.len) + "B </div>";
				
				if (document.getElementById("sai_arts"))
					document.getElementById("sai_arts").innerHTML = sai_arts;
				
			}

			if (jso.schema == "com-warmcat-sai-logs") {
				var s1 = atob(jso.log), s = hsanitize(s1), li,
					en = "", yo, dh, ce, tn = "";
					
				if (!tfirst)
					tfirst = jso.timestamp; 
				
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
				
				if (!cont[jso.channel] && jso.len)
					tn = ((jso.timestamp - tfirst) / 1000000).toFixed(4);

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
			}
		};

		sai.onclose = function(){
//			document.getElementById("title").innerHTML =
//						"Server Status (Disconnected)";
//			lws_gray_out(true,{"zindex":"499"});
			
			myVar = setTimeout(ws_open_sai, 10000);
		};
	} catch(exception) {
		alert("<p>Error" + exception);  
	}
}

/* stuff that has to be delayed until all the page assets are loaded */

window.addEventListener("load", function() {

	if (document.getElementById("noscript"))
		document.getElementById("noscript").display = "none";
		
	/* login form hidden success redirect */
	if (document.getElementById("success_redir"))
		document.getElementById("success_redir").value =
			window.location.href;
	ws_open_sai();
	aging();
	
	
	setInterval(function() {

	    var locked = document.body.scrollHeight -
	    	document.body.clientHeight <= document.body.scrollTop + 1;
	
	    if (locked)
	     document.body.scrollTop = document.body.scrollHeight -
	     	document.body.clientHeight;

	}, 500)
	
}, false);

}());

