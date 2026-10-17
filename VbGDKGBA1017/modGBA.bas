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
Public Declare Sub GBA_SetScreenBuffer3X Lib "gdkgba.dll" (ByVal pDIBBits As Long)
Public Declare Sub GBA_SetVideoFilter Lib "gdkgba.dll" (ByVal Mode As Long)


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
Public Const BI_RGB As Long = 0&
Public Const HALFTONE As Long = 4&     ' 부드러운 보간 확대 모드

' GBA 네이티브 해상도
Public Const GBA_WIDTH As Long = 240
Public Const GBA_HEIGHT As Long = 160
Public Const GBA_FRAME_BYTES As Long = GBA_WIDTH * GBA_HEIGHT * 4 ' 153,600 Bytes

' =============================================================================
' 전역 비디오 객체 변수 (480x320 Scale2x 버퍼용)
' =============================================================================
Public Const GBA_W_2X As Long = 480
Public Const GBA_H_2X As Long = 320
Public Const GBA_W_3X As Long = 720   ' 240 * 3
Public Const GBA_H_3X As Long = 480   ' 160 * 3

Public hMemDC   As Long
Public hDIB     As Long
Public hOldBmp  As Long
Public pDIBBits As Long ' CreateDIBSection이 할당한 실제 픽셀 메모리 주소!


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

' -----------------------------------------------------------------------------
' 1. 480x320 DIBSection 및 메모리 DC 초기화 (Form_Load에서 1회 호출)
' -----------------------------------------------------------------------------
Public Sub InitVideo3X(ByVal FormHDC As Long)
    Dim bmi As BITMAPINFO
    
    ' 기존 리소스가 있다면 정리
    CleanupVideo3X
    
    With bmi.bmiHeader
        .biSize = Len(bmi.bmiHeader)
        .biWidth = GBA_W_3X          ' 480 픽셀
        .biHeight = -GBA_H_3X        ' -320 픽셀 (음수 = 위에서 아래로 그리는 Top-Down DIB)
        .biPlanes = 1
        .biBitCount = 32             ' 32비트 (0x00RRGGBB)
        .biCompression = BI_RGB
        .biSizeImage = 720& * 480& * 4&
    End With
    
    ' 메모리 DC 생성 및 480x320 DIBSection 생성
    hMemDC = CreateCompatibleDC(FormHDC)
    hDIB = CreateDIBSection(FormHDC, bmi, DIB_RGB_COLORS, pDIBBits, 0&, 0&)
    hOldBmp = SelectObject(hMemDC, hDIB)
    
    ' [핵심!] C DLL에 pDIBBits 주소를 알려주면, C의 apply_scale2x()가
    ' 매 프레임마다 VB6의 DIB 메모리에 직접 그림을 그려 넣습니다! (CopyMemory 생략 가능)
    GBA_SetScreenBuffer3X pDIBBits
End Sub

' -----------------------------------------------------------------------------
' 2. StretchBlt로 화면에 출력 (메인 루프에서 매 프레임 호출)
' -----------------------------------------------------------------------------
Public Sub RenderScreenStretchBlt(ByVal TargetHDC As Long, _
                                  ByVal DstX As Long, ByVal DstY As Long, _
                                  ByVal DstW As Long, ByVal DstH As Long)
    If hMemDC = 0 Or pDIBBits = 0 Then Exit Sub
    
    ' (만약 GBA_SetScreenBuffer2X를 안 쓰고 수동 복사하고 싶다면 아래 주석 해제)
    ' CopyMemory pDIBBits, GBA_GetScreen2X(), GBA_W_2X * GBA_H_2X * 4&
    
    ' StretchBlt 확대 시 픽셀이 흐릿하게 깨지는 것을 방지
    SetStretchBltMode TargetHDC, COLORONCOLOR
    
    ' 480x320 메모리 DC(hMemDC)의 화면을 폼(TargetHDC)의 원하는 크기로 StretchBlt 출력!
    StretchBlt TargetHDC, _
               DstX, DstY, DstW, DstH, _
               hMemDC, 0, 0, GBA_W_3X, GBA_H_3X, _
               vbSrcCopy
End Sub

' -----------------------------------------------------------------------------
' 3. 종료 시 GDI 리소스 해제 (Form_Unload에서 호출)
' -----------------------------------------------------------------------------
Public Sub CleanupVideo3X()
    GBA_SetScreenBuffer3X 0&
    
    If hMemDC <> 0 Then
        If hOldBmp <> 0 Then
            SelectObject hMemDC, hOldBmp
            hOldBmp = 0
        End If
        If hDIB <> 0 Then
            DeleteObject hDIB
            hDIB = 0
        End If
        DeleteDC hMemDC
        hMemDC = 0
    End If
    pDIBBits = 0
End Sub
