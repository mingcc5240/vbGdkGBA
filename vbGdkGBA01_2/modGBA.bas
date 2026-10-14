Attribute VB_Name = "modGBA"
Option Explicit

' --- Win32 GDI & Memory API 선언 ---
Public Declare Function CreateCompatibleDC Lib "gdi32" (ByVal hDC As Long) As Long
Public Declare Function DeleteDC Lib "gdi32" (ByVal hDC As Long) As Long
Public Declare Function CreateDIBSection Lib "gdi32" ( _
    ByVal hDC As Long, _
    ByRef pbmi As BITMAPINFO, _
    ByVal iUsage As Long, _
    ByRef ppvBits As Long, _
    ByVal hSection As Long, _
    ByVal dwOffset As Long) As Long
Public Declare Function SelectObject Lib "gdi32" (ByVal hDC As Long, ByVal hObject As Long) As Long
Public Declare Function DeleteObject Lib "gdi32" (ByVal hObject As Long) As Long
Public Declare Function BitBlt Lib "gdi32" ( _
    ByVal hDestDC As Long, ByVal X As Long, ByVal Y As Long, _
    ByVal nWidth As Long, ByVal nHeight As Long, _
    ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, _
    ByVal dwRop As Long) As Long
Public Declare Sub CopyMemory Lib "kernel32" Alias "RtlMoveMemory" ( _
    ByRef Destination As Any, ByRef Source As Any, ByVal Length As Long)

Public Declare Function StretchBlt Lib "gdi32" ( _
    ByVal hDestDC As Long, ByVal X As Long, ByVal Y As Long, _
    ByVal nWidth As Long, ByVal nHeight As Long, _
    ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, _
    ByVal nSrcWidth As Long, ByVal nSrcHeight As Long, _
    ByVal dwRop As Long) As Long

Public Declare Function SetStretchBltMode Lib "gdi32" ( _
    ByVal hDC As Long, ByVal nStretchMode As Long) As Long

Public Const COLORONCOLOR As Long = 3 ' 도트 픽셀을 가장 선명하게 유지하는 모드
' --- Win32 고해상도 타이머 & 슬립 API ---
Public Declare Function QueryPerformanceCounter Lib "kernel32" (ByRef lpPerformanceCount As Currency) As Long
Public Declare Function QueryPerformanceFrequency Lib "kernel32" (ByRef lpFrequency As Currency) As Long
Public Declare Sub Sleep Lib "kernel32" (ByVal dwMilliseconds As Long)
Public Declare Sub ZeroMemory Lib "kernel32.dll" Alias "RtlZeroMemory" ( _
    ByVal Destination As Long, ByVal Length As Long)

' --- GBA DLL API 선언 ---
Public Declare Function GBA_Init Lib "gdkgba.dll" (ByVal rom_path As String) As Long
Public Declare Function GBA_RunFrame Lib "gdkgba.dll" () As Long
Public Declare Sub GBA_SetKeys Lib "gdkgba.dll" (ByVal keys As Integer)
Public Declare Sub GBA_Close Lib "gdkgba.dll" ()
Public Declare Sub GBA_Reset Lib "gdkgba.dll" ()


' --- DIB 구조체 정의 ---
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

Public Const SRCCOPY As Long = &HCC0020
Public Const DIB_RGB_COLORS As Long = 0

' GBA 네이티브 해상도
Public Const GBA_WIDTH As Long = 240
Public Const GBA_HEIGHT As Long = 160
Public Const GBA_FRAME_BYTES As Long = GBA_WIDTH * GBA_HEIGHT * 4 ' 153,600 Bytes

' --- Win32 OpenFileName API 선언 (OCX 미사용) ---
Private Type OPENFILENAME
    lStructSize As Long
    hWndOwner As Long
    hInstance As Long
    lpstrFilter As String
    lpstrCustomFilter As String
    nMaxCustFilter As Long
    nFilterIndex As Long
    lpstrFile As String
    nMaxFile As Long
    lpstrFileTitle As String
    nMaxFileTitle As Long
    lpstrInitialDir As String
    lpstrTitle As String
    Flags As Long
    nFileOffset As Integer
    nFileExtension As Integer
    lpstrDefExt As String
    lCustData As Long
    lpfnHook As Long
    lpTemplateName As String
End Type

Private Declare Function GetOpenFileName Lib "comdlg32.dll" Alias "GetOpenFileNameA" ( _
    ByRef pOpenfilename As OPENFILENAME) As Long

Private Const OFN_FILEMUSTEXIST As Long = &H1000
Private Const OFN_PATHMUSTEXIST As Long = &H800
Private Const OFN_HIDEREADONLY As Long = &H4

Public Const WHDR_DONE As Long = &H1        ' 버퍼 재생 완료 (&H00000001)
Public Const WHDR_PREPARED As Long = &H2    ' waveOutPrepareHeader 완료 (&H00000002)
Public Const WHDR_BEGINLOOP As Long = &H4   ' 루프 시작 (&H00000004)
Public Const WHDR_ENDLOOP As Long = &H8     ' 루프 종료 (&H00000008)
Public Const WHDR_INQUEUE As Long = &H10    ' 드라이버 출력 큐에서 대기 중 (&H00000010)


    
''' <summary>
''' OCX 없이 Windows 표준 파일 선택 대화상자를 엽니다.
''' </summary>
Public Function ShowOpenFileDialog(ByVal hWndOwner As Long) As String
    Dim ofn As OPENFILENAME
    Dim sFile As String
    Dim sFilter As String
    
    ' 널 문자로 끝나는 필터 문자열 구성
    sFilter = "GBA ROM Files (*.gba;*.bin)" & vbNullChar & "*.gba;*.bin" & vbNullChar & _
              "All Files (*.*)" & vbNullChar & "*.*" & vbNullChar & vbNullChar
    
    sFile = String$(260, vbNullChar)
    
    With ofn
        .lStructSize = Len(ofn)
        .hWndOwner = hWndOwner
        .lpstrFilter = sFilter
        .nFilterIndex = 1
        .lpstrFile = sFile
        .nMaxFile = Len(sFile)
        .lpstrFileTitle = String$(260, vbNullChar)
        .nMaxFileTitle = 260
        .lpstrInitialDir = App.Path
        .lpstrTitle = "GBA ROM 파일 선택"
        .Flags = OFN_FILEMUSTEXIST Or OFN_PATHMUSTEXIST Or OFN_HIDEREADONLY
    End With
    
    If GetOpenFileName(ofn) <> 0 Then
        ' 반환된 문자열에서 널 문자(vbNullChar) 이전의 순수 경로만 추출
        ShowOpenFileDialog = Left$(ofn.lpstrFile, InStr(ofn.lpstrFile, vbNullChar) - 1)
    Else
        ShowOpenFileDialog = "" ' 취소 버튼을 누른 경우
    End If
End Function

