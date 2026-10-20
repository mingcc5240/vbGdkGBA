Attribute VB_Name = "Module1"
Option Explicit

' --- 1. 프로세스 및 윈도우 제어 API ---
Public Declare Function EnumWindows Lib "user32" (ByVal lpEnumFunc As Long, ByVal lParam As Long) As Long
Public Declare Function GetWindowThreadProcessId Lib "user32" (ByVal hWnd As Long, lpdwProcessId As Long) As Long
Public Declare Function IsWindowVisible Lib "user32" (ByVal hWnd As Long) As Long
Public Declare Function GetWindow Lib "user32" (ByVal hWnd As Long, ByVal wCmd As Long) As Long
Public Declare Function SetParent Lib "user32" (ByVal hWndChild As Long, ByVal hWndNewParent As Long) As Long
Public Declare Function GetWindowLong Lib "user32" Alias "GetWindowLongA" (ByVal hWnd As Long, ByVal nIndex As Long) As Long
Public Declare Function SetWindowLong Lib "user32" Alias "SetWindowLongA" (ByVal hWnd As Long, ByVal nIndex As Long, ByVal dwNewLong As Long) As Long
Public Declare Function MoveWindow Lib "user32" (ByVal hWnd As Long, ByVal x As Long, ByVal y As Long, ByVal nWidth As Long, ByVal nHeight As Long, ByVal bRepaint As Long) As Long
Public Declare Function SetWindowPos Lib "user32" (ByVal hWnd As Long, ByVal hWndInsertAfter As Long, ByVal x As Long, ByVal y As Long, ByVal cx As Long, ByVal cy As Long, ByVal wFlags As Long) As Long
Public Declare Function PostMessage Lib "user32" Alias "PostMessageA" (ByVal hWnd As Long, ByVal wMsg As Long, ByVal wParam As Long, ByVal lParam As Long) As Long
Public Declare Function MapVirtualKey Lib "user32" Alias "MapVirtualKeyA" (ByVal wCode As Long, ByVal wMapType As Long) As Long

' --- 2. GDI 캡처 및 1차원 배열 복사 API ---
Public Declare Function GetDC Lib "user32" (ByVal hWnd As Long) As Long
Public Declare Function ReleaseDC Lib "user32" (ByVal hWnd As Long, ByVal hDC As Long) As Long
Public Declare Function CreateCompatibleDC Lib "gdi32" (ByVal hDC As Long) As Long
Public Declare Function DeleteDC Lib "gdi32" (ByVal hDC As Long) As Long
Public Declare Function SelectObject Lib "gdi32" (ByVal hDC As Long, ByVal hObject As Long) As Long
Public Declare Function DeleteObject Lib "gdi32" (ByVal hObject As Long) As Long

Public Declare Function BitBlt Lib "gdi32" (ByVal hDestDC As Long, ByVal x As Long, ByVal y As Long, ByVal nWidth As Long, ByVal nHeight As Long, ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, ByVal dwRop As Long) As Long
Public Declare Function StretchBlt Lib "gdi32" (ByVal hDestDC As Long, ByVal x As Long, ByVal y As Long, ByVal nWidth As Long, ByVal nHeight As Long, ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, ByVal nSrcWidth As Long, ByVal nSrcHeight As Long, ByVal dwRop As Long) As Long
Public Declare Function SetStretchBltMode Lib "gdi32" (ByVal hDC As Long, ByVal nStretchMode As Long) As Long

Public Type BITMAPINFOHEADER
    biSize As Long
    biWidth As Long
    biHeight As Long
    biPlanes As Integer
    biBitCount As Integer
    biCompression As Long
    biSizeImage As Long
    biXPelsPerMeter As Long
    biYPelsPerMeter As Long
    biClrUsed As Long
    biClrImportant As Long
End Type

Public Type BITMAPINFO
    bmiHeader As BITMAPINFOHEADER
End Type

Public Declare Function CreateDIBSection Lib "gdi32" ( _
    ByVal hDC As Long, pBitmapInfo As BITMAPINFO, ByVal un As Long, _
    lplpVoid As Long, ByVal handle As Long, ByVal dw As Long) As Long

Public Declare Function GetDIBits Lib "gdi32" ( _
    ByVal aHDC As Long, ByVal hBitmap As Long, ByVal nStartScan As Long, ByVal nNumScans As Long, _
    lpBits As Any, lpBI As BITMAPINFO, ByVal wUsage As Long) As Long

Public Declare Function StretchDIBits Lib "gdi32" ( _
    ByVal hDC As Long, ByVal x As Long, ByVal y As Long, ByVal dx As Long, ByVal dy As Long, _
    ByVal SrcX As Long, ByVal SrcY As Long, ByVal wSrcWidth As Long, ByVal wSrcHeight As Long, _
    lpBits As Any, lpBitsInfo As BITMAPINFO, ByVal wUsage As Long, ByVal dwRop As Long) As Long

' 상수 정의
Public Const DIB_RGB_COLORS As Long = 0
Public Const BI_RGB As Long = 0
Public Const SRCCOPY As Long = &HCC0020
Public Const COLORONCOLOR As Long = 3

Public Const GWL_STYLE As Long = -16
Public Const WS_CAPTION As Long = &HC00000
Public Const WS_THICKFRAME As Long = &H40000
Public Const WS_CHILD As Long = &H40000000
Public Const WS_POPUP As Long = &H80000000
Public Const SWP_NOZORDER As Long = &H4
Public Const SWP_NOMOVE As Long = &H2
Public Const SWP_NOSIZE As Long = &H1
Public Const SWP_FRAMECHANGED As Long = &H20
Public Const GW_OWNER As Long = 4

Public Const WM_KEYDOWN As Long = &H100
Public Const WM_KEYUP As Long = &H101
Public Const WM_CLOSE As Long = &H10
Public Const WM_ACTIVATE As Long = &H6
Public Const WA_ACTIVE As Long = 1

Public g_TargetPID As Long
Public g_mgbaHwnd As Long

Public Type DIBBuffer
    hDC As Long
    hDIB As Long
    hOldBmp As Long
    pBits As Long
    Width As Long
    Height As Long
End Type

Public Function CreateFastDIB(ByVal Width As Long, ByVal Height As Long) As DIBBuffer
    Dim bmi As BITMAPINFO
    Dim buf As DIBBuffer
    
    buf.Width = Width
    buf.Height = Height
    buf.hDC = CreateCompatibleDC(0)
    
    With bmi.bmiHeader
        .biSize = Len(bmi.bmiHeader)
        .biWidth = Width
        .biHeight = -Height ' Top-Down (좌상단이 0번 인덱스)
        .biPlanes = 1
        .biBitCount = 32
        .biCompression = BI_RGB
    End With
    
    buf.hDIB = CreateDIBSection(buf.hDC, bmi, DIB_RGB_COLORS, buf.pBits, 0, 0)
    buf.hOldBmp = SelectObject(buf.hDC, buf.hDIB)
    CreateFastDIB = buf
End Function

Public Sub DestroyFastDIB(ByRef buf As DIBBuffer)
    If buf.hDC <> 0 Then
        If buf.hOldBmp <> 0 Then SelectObject buf.hDC, buf.hOldBmp
        If buf.hDIB <> 0 Then DeleteObject buf.hDIB
        DeleteDC buf.hDC
    End If
    buf.hDC = 0
    buf.hDIB = 0
End Sub

Public Function EnumWindowsProc(ByVal hWnd As Long, ByVal lParam As Long) As Long
    Dim pid As Long
    GetWindowThreadProcessId hWnd, pid
    If pid = g_TargetPID Then
        If IsWindowVisible(hWnd) <> 0 And GetWindow(hWnd, GW_OWNER) = 0 Then
            g_mgbaHwnd = hWnd
            EnumWindowsProc = 0
            Exit Function
        End If
    End If
    EnumWindowsProc = 1
End Function

Public Function MakeKeyLParam(ByVal VirtualKey As Long, ByVal IsKeyUp As Boolean) As Long
    Dim scanCode As Long
    scanCode = MapVirtualKey(VirtualKey, 0)
    MakeKeyLParam = 1 Or (scanCode * &H10000)
    Select Case VirtualKey
        Case vbKeyUp, vbKeyDown, vbKeyLeft, vbKeyRight, vbKeyInsert, vbKeyDelete, vbKeyHome, vbKeyEnd, vbKeyPageUp, vbKeyPageDown
            MakeKeyLParam = MakeKeyLParam Or &H1000000
    End Select
    If IsKeyUp Then MakeKeyLParam = MakeKeyLParam Or &HC0000000
End Function

