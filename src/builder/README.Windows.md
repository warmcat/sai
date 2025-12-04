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

## Installation

To ensure the service runs correctly with all dependencies (DLLs) and correct permissions, perform a system-wide installation. This requires Administrator privileges.

Open a Command Prompt **as Administrator**:

```cmd
cd sai/sai-builder/build
cmake --install . --config Release
```

By default, this installs to `C:\Program Files (x86)\sai`. The executable will be in `C:\Program Files (x86)\sai\bin\sai-builder.exe`, and all necessary DLLs (libwebsockets, OpenSSL) will be copied there.

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

### 3. Grant File Access

Since the service runs as the `sai` user, it must have permission to read and execute the files in your installation and configuration directories.

Open a Command Prompt **as Administrator** and run:

1.  Grant access to the installation directory (e.g., `C:\Program Files (x86)\sai`):
    ```cmd
    icacls "C:\Program Files (x86)\sai" /grant sai:(OI)(CI)RX /T
    ```
2.  Grant access to your configuration directory (e.g., `C:\path\to\sai\etc`):
    ```cmd
    icacls "C:\path\to\sai\etc" /grant sai:(OI)(CI)RX /T
    ```

*   `/grant sai:(OI)(CI)RX`: Grants Read and Execute (RX) permissions to user `sai`. `(OI)(CI)` ensures inheritance.
*   `/T`: Applies recursively.

### 4. Install the Service

Open a Command Prompt **as Administrator**.

Run the following `sc create` command. **Note:** The space after the `=` sign is mandatory.

```cmd
sc create SaiBuilder ^
  binPath= "C:\Program Files (x86)\sai\bin\sai-builder.exe --service -c C:\path\to\sai\etc\conf" ^
  DisplayName= "Sai Builder" ^
  start= auto ^
  obj= ".\sai" ^
  password= "your_password_here"
```

*   `binPath`: The installed executable path + arguments.
*   `obj`: The user account to run as (`.\username`).
*   `password`: The user's password.

### 5. Manage the Service

Start and stop the service as Administrator:

```cmd
sc start SaiBuilder
sc stop SaiBuilder
```
