# sai-builder on Windows

## Building

Building on Windows follows the standard CMake flow. You can perform the build as a standard user (e.g., "andy").

```cmd
cd sai/sai-builder
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

This will produce `sai-builder.exe` in `build\Release`.

## Setup for Service Execution

To run `sai-builder` as a Windows Service under a specific unprivileged user (e.g., `.\sai`), follow these steps.

### 1. Create the User

If the `sai` user does not exist, create it. Open a Command Prompt **as Administrator**.

```cmd
net user sai <password> /add /passwordchg:no /expires:never
```

### 2. Grant Privileges

The `sai` user needs permission to:
1.  **Log on as a Service**: Windows usually grants this automatically when you register the service with a specific user, but you can check it in `secpol.msc`.
2.  **Shut down the system**: If you want the builder to handle power management (suspend/shutdown).

To grant rights:
1.  Run `secpol.msc` (Local Security Policy).
2.  Go to **Local Policies** -> **User Rights Assignment**.
3.  Find **Shut down the system**.
4.  Double-click, click **Add User or Group**, type `sai`, check Names, and OK.
5.  (Optional) Ensure **Log on as a service** also includes `sai`.

### 3. Install the Service

Open a Command Prompt **as Administrator** (Right-click Start -> Command Prompt (Admin) or PowerShell (Admin)).

Run the following `sc create` command. **Note:** The space after the `=` sign in options like `binPath=` is mandatory.

Replace the paths and password with your actual values.

```cmd
sc create SaiBuilder ^
  binPath= "C:\path\to\sai\sai-builder\build\Release\sai-builder.exe --service -c C:\path\to\sai\etc\conf" ^
  DisplayName= "Sai Builder" ^
  start= auto ^
  obj= ".\sai" ^
  password= "your_password_here"
```

*   `binPath`: The full path to the executable *plus* the arguments needed (like `--service` and the config file path).
*   `obj`: The user account to run as (`.\username`).
*   `password`: The user's password.

### 4. Manage the Service

You can now start and stop the builder using standard Windows commands (Administrator required):

```cmd
sc start SaiBuilder
sc stop SaiBuilder
```

To view logs or status, check the standard `sai-builder` logs (configured in your JSON config) or the Event Viewer (Application log) if the service fails to start immediately.

### 5. Updating the Builder

To update the executable:
1.  Stop the service: `sc stop SaiBuilder`
2.  Rebuild as your normal user ("andy").
3.  Start the service: `sc start SaiBuilder`

### 6. Troubleshooting

If the service fails to start:
1.  Check that the `sai` user has "Log on as a service" rights.
2.  Verify the paths in `binPath` are correct and accessible by the `sai` user.
3.  Ensure `sai-builder.exe` arguments include `--service`.
