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
1.  **Log on as a Service**: Windows typically grants this automatically when you register the service, but it's good to verify.
2.  **Shut down the system**: If you want the builder to handle power management (suspend/shutdown).

#### Method A: Windows Pro / Enterprise (using secpol.msc)

1.  Run `secpol.msc` (Local Security Policy).
2.  Go to **Local Policies** -> **User Rights Assignment**.
3.  Find **Shut down the system**.
4.  Double-click, click **Add User or Group**, type `sai`, check Names, and OK.
5.  Find **Log on as a service**.
6.  Ensure `sai` is listed there as well.

#### Method B: Windows Home (via PowerShell)

Windows Home edition does not include `secpol.msc`. You can use the following PowerShell script to grant the necessary rights.

1.  Open **PowerShell as Administrator**.
2.  Copy and paste the following script into the window and press Enter:

```powershell
$definition = @"
using System;
using System.Runtime.InteropServices;
using System.Security.Principal;

public class LsaUtility
{
    [DllImport("advapi32.dll", PreserveSig = true)]
    private static extern UInt32 LsaOpenPolicy(ref LSA_UNICODE_STRING SystemName, ref LSA_OBJECT_ATTRIBUTES ObjectAttributes, Int32 DesiredAccess, out IntPtr PolicyHandle);

    [DllImport("advapi32.dll", PreserveSig = true)]
    private static extern UInt32 LsaAddAccountRights(IntPtr PolicyHandle, byte[] AccountSid, LSA_UNICODE_STRING[] UserRights, Int32 CountOfRights);

    [DllImport("advapi32.dll", PreserveSig = true)]
    private static extern UInt32 LsaClose(IntPtr PolicyHandle);

    [StructLayout(LayoutKind.Sequential)]
    private struct LSA_UNICODE_STRING
    {
        public UInt16 Length;
        public UInt16 MaximumLength;
        public IntPtr Buffer;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct LSA_OBJECT_ATTRIBUTES
    {
        public Int32 Length;
        public IntPtr RootDirectory;
        public IntPtr ObjectName;
        public UInt32 Attributes;
        public IntPtr SecurityDescriptor;
        public IntPtr SecurityQualityOfService;
    }

    public static void GrantPrivilege(string username, string privilegeName)
    {
        IntPtr policyHandle = IntPtr.Zero;
        LSA_OBJECT_ATTRIBUTES objAttributes = new LSA_OBJECT_ATTRIBUTES();
        LSA_UNICODE_STRING systemName = new LSA_UNICODE_STRING();

        uint result = LsaOpenPolicy(ref systemName, ref objAttributes, 0x0800 /* POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT */, out policyHandle);
        if (result != 0) { throw new Exception("Error opening LSA policy: " + result); }

        try
        {
            NTAccount account = new NTAccount(username);
            SecurityIdentifier sid = (SecurityIdentifier)account.Translate(typeof(SecurityIdentifier));
            byte[] sidBytes = new byte[sid.BinaryLength];
            sid.GetBinaryForm(sidBytes, 0);

            LSA_UNICODE_STRING[] rights = new LSA_UNICODE_STRING[1];
            rights[0] = new LSA_UNICODE_STRING();
            rights[0].Length = (UInt16)(privilegeName.Length * 2);
            rights[0].MaximumLength = (UInt16)(rights[0].Length + 2);
            rights[0].Buffer = Marshal.StringToHGlobalUni(privilegeName);

            result = LsaAddAccountRights(policyHandle, sidBytes, rights, 1);
            if (result != 0) { throw new Exception("Error adding rights: " + result); }

            Console.WriteLine("Granted " + privilegeName + " to " + username);
        }
        finally
        {
            LsaClose(policyHandle);
        }
    }
}
"@

Add-Type -TypeDefinition $definition
[LsaUtility]::GrantPrivilege("sai", "SeShutdownPrivilege")
[LsaUtility]::GrantPrivilege("sai", "SeServiceLogonRight")
```

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
