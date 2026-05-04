# Sai Watcher System

The Sai Watcher system allows the CI server to monitor external asynchronous services (like Coverity, SonarCloud, etc.) via periodic HTML screenscraping. This is useful for services that provide status updates at a public URL after a build or upload is completed.

## How it Works

1.  **Trigger**: A builder task reports a public status URL by printing `SAI_WATCH_URL: <url>` to its log (stdout or stderr).
2.  **Registration**: The server detects this prefix, identifies the service type by matching the URL against configured patterns, and creates a record in the central `watchers` table.
3.  **Polling**: A background timer periodically fetches the HTML from the URL using Secure Streams.
4.  **Scraping**: The server applies configured rules (prefix/suffix/anchor) to extract metrics from the HTML.
5.  **UI Rendering**: The extracted metrics are sent to the browser, which renders them dynamically according to UI rules provided in the service configuration.

## Configuration

Watchers are defined in JSON files located in `/etc/sai/server/conf.d/`. Each file can contain a `"watchers"` array.

### Service Object Fields

| Field | Type | Description |
| :--- | :--- | :--- |
| `name` | string | Unique name for the service (e.g., "coverity"). Used to find icons in `assets/watchers/<name>/icon.svg`. |
| `match` | string | Substring to match against the reported URL to identify this service. |
| `rules` | array | List of scraping rules to extract data from HTML. |
| `ui` | array | List of rendering rules for the Web UI. |

### Scraping Rule Fields (`rules`)

| Field | Type | Description |
| :--- | :--- | :--- |
| `label` | string | Key name for the extracted value in the metrics JSON. |
| `prefix` | string | HTML string immediately preceding the value. |
| `suffix` | string | HTML string immediately following the value. |
| `anchor` | string | (Optional) HTML string to find first before looking for the prefix. |
| `final` | boolean | If this rule matches, the watcher transitions to the `FINISHED` state and stops polling. |

### UI Rule Fields (`ui`)

| Field | Type | Description |
| :--- | :--- | :--- |
| `label` | string | Human-readable label displayed in the UI. |
| `key` | string | The metric key (from `rules.label`) to display. |
| `warn_if_gt` | number | (Optional) Value above which the metric is highlighted as a warning. |
| `fail_if_gt` | number | (Optional) Value above which the metric is highlighted as a failure. |

## Example: Coverity

To configure a watcher for Coverity, create a file like `/etc/sai/server/conf.d/coverity.json`:

```json
{
  "watchers": [
    {
      "name": "coverity",
      "match": "scan.coverity.com/projects/",
      "rules": [
        {
          "label": "status",
          "prefix": "Last Build Status:</td><td>",
          "suffix": "</td>"
        },
        {
          "label": "density",
          "prefix": "Defect Density:</td><td>",
          "suffix": "</td>"
        },
        {
          "label": "defects",
          "prefix": "Outstanding Defects:</td><td>",
          "suffix": "</td>"
        },
        {
          "label": "passed",
          "prefix": "Last Build Status:</td><td>Passed",
          "suffix": "</td>",
          "final": true
        }
      ],
      "ui": [
        {
          "label": "Status",
          "key": "status"
        },
        {
          "label": "Density",
          "key": "density"
        },
        {
          "label": "Defects",
          "key": "defects",
          "warn_if_gt": 0,
          "fail_if_gt": 10
        }
      ]
    }
  ]
}
```

### Triggering from `.sai.json`

In your task steps, after a successful upload, simply echo the status URL:

```json
"steps": [
  {
    "name": "upload",
    "run": "<upload commands> ... && echo \"SAI_WATCH_URL: https://scan.coverity.com/projects/my-project\""
  }
]
```

## Assets

Place a service icon at `assets/watchers/<name>/icon.svg` on the server to have it appear in the dashboard.
