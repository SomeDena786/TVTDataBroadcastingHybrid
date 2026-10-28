# TVTestの子ウィンドウ一覧 (クラス名, 拡張スタイル, 可視, 矩形)
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
  public static List<string> List(IntPtr root) {
    var list = new List<string>();
    EnumChildWindows(root, (h, l) => {
      var sb = new StringBuilder(256); GetClassName(h, sb, 256);
      RECT r; GetWindowRect(h, out r);
      list.Add(String.Format("{0:X8} parent={1:X8} {2,-40} ex={3:X8} vis={4} [{5},{6},{7},{8}]", h.ToInt64(), GetParent(h).ToInt64(), sb, GetWindowLong(h, -20), IsWindowVisible(h), r.L, r.T, r.R, r.B));
      return true; }, IntPtr.Zero);
    return list;
  }
}
"@
$p = Get-Process TVTest | Select-Object -First 1
[CW]::List($p.MainWindowHandle)
