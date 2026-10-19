VERSION 5.00
Begin VB.Form Form1 
   Caption         =   "mGBA with VB6.0"
   ClientHeight    =   9540
   ClientLeft      =   60
   ClientTop       =   405
   ClientWidth     =   15060
   LinkTopic       =   "Form1"
   ScaleHeight     =   636
   ScaleMode       =   3  '픽셀
   ScaleWidth      =   1004
   StartUpPosition =   3  'Windows 기본값
   Begin VB.Timer tmrCopy 
      Enabled         =   0   'False
      Interval        =   16
      Left            =   10920
      Top             =   7560
   End
   Begin VB.PictureBox PicClone 
      Height          =   4800
      Left            =   7560
      ScaleHeight     =   316
      ScaleMode       =   3  '픽셀
      ScaleWidth      =   476
      TabIndex        =   2
      Top             =   120
      Width           =   7200
   End
   Begin VB.Timer tmrFindWindow 
      Enabled         =   0   'False
      Interval        =   100
      Left            =   7560
      Top             =   8280
   End
   Begin VB.CommandButton cmdLaunch 
      Caption         =   "mGBA Launch "
      Height          =   495
      Left            =   600
      TabIndex        =   1
      Top             =   8040
      Width           =   2415
   End
   Begin VB.PictureBox PicScreen 
      Height          =   4800
      Left            =   120
      ScaleHeight     =   4740
      ScaleWidth      =   7140
      TabIndex        =   0
      Top             =   120
      Width           =   7200
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

Private Const ROM_FILENAME As String = "metalslug.gba"

Private Const GBA_W As Long = 240
Private Const GBA_H As Long = 160
Private Const MENU_HEIGHT As Long = 21

' 240 * 160 = 38,400개의 픽셀을 담을 1차원 배열 (0번 인덱스부터 38399번까지)
Private m_Pixels() As Long
Private m_BMI As BITMAPINFO
Private m_CapBuf As DIBBuffer

' 현재 적용 중인 픽셀 조작 모드 (0: 원본, 1: 스캔라인, 2: 색상반전, 3: 고전 게임보이 녹색필터)
Private m_FilterMode As Integer

Private Function GetAppPath(ByVal sFileName As String) As String
    If Right$(App.Path, 1) = "\" Then
        GetAppPath = App.Path & sFileName
    Else
        GetAppPath = App.Path & "\" & sFileName
    End If
End Function

Private Sub Form_Load()
    Me.KeyPreview = True
    
    PicScreen.ScaleMode = vbPixels
    PicClone.ScaleMode = vbPixels
    PicClone.AutoRedraw = False
    SetStretchBltMode PicClone.hDC, COLORONCOLOR
    
    ' 1. 캡처용 임시 버퍼(240x160) 생성
    m_CapBuf = CreateFastDIB(GBA_W, GBA_H)
    
    ' 2. 픽셀을 담을 1차원 배열 크기 할당 (0 ~ 38,399)
    ReDim m_Pixels(0 To (GBA_W * GBA_H) - 1) As Long
    
    ' 3. 1차원 배열용 BITMAPINFO 구조체 초기화 (Top-Down 32비트 RGB)
    With m_BMI.bmiHeader
        .biSize = Len(m_BMI.bmiHeader)
        .biWidth = GBA_W
        .biHeight = -GBA_H ' 음수(-): 0번 인덱스가 좌상단(0,0)이 되도록 설정
        .biPlanes = 1
        .biBitCount = 32   ' 1픽셀 = 4바이트(Long) = &H00RRGGBB
        .biCompression = BI_RGB
    End With
    
    m_FilterMode = 1 ' 기본값: 스캔라인 효과 체험
End Sub

Private Sub cmdLaunch_Click()
    Dim mgbaPath As String
    Dim romPath As String
    Dim cmdLine As String
    
    mgbaPath = GetAppPath("mGBA.exe")
    
    If Dir$(mgbaPath) = "" Then
        MsgBox "현재 폴더에서 mGBA.exe를 찾을 수 없습니다." & vbCrLf & _
               "경로: " & mgbaPath, vbExclamation, "파일 없음"
        Exit Sub
    End If
    
    If ROM_FILENAME <> "" Then
        romPath = GetAppPath(ROM_FILENAME)
        If Dir$(romPath) <> "" Then
            cmdLine = """" & mgbaPath & """ """ & romPath & """"
        Else
            cmdLine = """" & mgbaPath & """"
        End If
    Else
        cmdLine = """" & mgbaPath & """"
    End If
    
    If g_mgbaHwnd <> 0 Then
        tmrCopy.Enabled = False
        PostMessage g_mgbaHwnd, WM_CLOSE, 0, 0
        g_mgbaHwnd = 0
    End If
    
    m_FilterMode = 0
    
    On Error GoTo ErrHandler
    g_TargetPID = Shell(cmdLine, vbNormalFocus)
    
    tmrFindWindow.Interval = 100
    tmrFindWindow.Enabled = True
    
    PicScreen.SetFocus
    Exit Sub

ErrHandler:
    MsgBox "mGBA 실행 실패! 오류 내용: " & Err.Description, vbCritical
End Sub

Private Sub tmrFindWindow_Timer()
    EnumWindows AddressOf EnumWindowsProc, 0
    
    If g_mgbaHwnd <> 0 Then
        tmrFindWindow.Enabled = False
        EmbedWindow g_mgbaHwnd
        
        ' 임베딩 완료 직후 고속 화면 복사 타이머 가동 (16ms = 약 60FPS)
        tmrCopy.Interval = 16
        tmrCopy.Enabled = True
    End If
End Sub

Private Sub EmbedWindow(ByVal hWnd As Long)
    Dim lStyle As Long
    
    lStyle = GetWindowLong(hWnd, GWL_STYLE)
    lStyle = lStyle And Not (WS_CAPTION Or WS_THICKFRAME Or WS_POPUP)
    lStyle = lStyle Or WS_CHILD
    SetWindowLong hWnd, GWL_STYLE, lStyle
    
    SetParent hWnd, PicScreen.hWnd
    SetWindowPos hWnd, 0, 0, 0, 0, 0, SWP_NOZORDER Or SWP_NOMOVE Or SWP_NOSIZE Or SWP_FRAMECHANGED
    MoveWindow hWnd, 0, 0, PicScreen.ScaleWidth, PicScreen.ScaleHeight, 1
    
    PostMessage hWnd, WM_ACTIVATE, WA_ACTIVE, 0
    PicScreen.SetFocus
End Sub

' 고속 화면 복사 타이머 이벤트
Private Sub tmrCopy_Timer()
   If g_mgbaHwnd = 0 Then Exit Sub
    
    Dim hSrcDC As Long
    Dim srcW As Long, srcH As Long
    Dim i As Long, x As Long, y As Long
    Dim c As Long, r As Long, g As Long, b As Long, gray As Long
    
    ' [1단계] mGBA 화면에서 메뉴바(21픽셀)를 뺀 게임 화면을 m_CapBuf(240x160)로 캡처
    hSrcDC = GetDC(g_mgbaHwnd)
    If hSrcDC = 0 Then Exit Sub
    
    srcW = PicScreen.ScaleWidth
    srcH = PicScreen.ScaleHeight - MENU_HEIGHT
    
    SetStretchBltMode m_CapBuf.hDC, COLORONCOLOR
    StretchBlt m_CapBuf.hDC, 0, 0, GBA_W, GBA_H, hSrcDC, 0, MENU_HEIGHT, srcW, srcH, SRCCOPY
    ReleaseDC g_mgbaHwnd, hSrcDC
    
    ' [2단계] 캡처된 이미지를 VB6의 1차원 배열 m_Pixels(0)으로 단번에 복사!
    GetDIBits m_CapBuf.hDC, m_CapBuf.hDIB, 0, GBA_H, m_Pixels(0), m_BMI, DIB_RGB_COLORS
    
    ' =========================================================================
    ' [3단계] 1차원 배열(m_Pixels) 직접 조작하기!
    ' * 2차원 좌표 (x, y)와 1차원 인덱스(i)의 관계: i = y * 240 + x
    ' * 각 원소 m_Pixels(i)에는 &H00RRGGBB 형태로 색상값이 들어있습니다.
    ' =========================================================================
    Select Case m_FilterMode
    
        Case 1 ' [효과 1] 레트로 CRT 스캔라인 (홀수 줄마다 밝기를 50%로 줄임)
            For y = 1 To GBA_H - 1 Step 2
                For x = 0 To GBA_W - 1
                    i = y * GBA_W + x
                    ' 각 RGB 채널을 2로 나누는 초고속 비트 마스킹 연산
                    m_Pixels(i) = (m_Pixels(i) \ 2) And &H7F7F7F
                Next x
            Next y
            
        Case 2 ' [효과 2] 색상 반전 (네거티브 필름 효과)
            For i = 0 To (GBA_W * GBA_H) - 1
                m_Pixels(i) = m_Pixels(i) Xor &HFFFFFF
            Next i
            
        Case 3 ' [효과 3] 1989년 초대 게임보이(DMG) 녹색 흑백 필터
            For i = 0 To (GBA_W * GBA_H) - 1
                c = m_Pixels(i)
                r = (c And &HFF0000) \ &H10000
                g = (c And &HFF00&) \ &H100&
                b = (c And &HFF&)
                
                ' 평균 밝기(Grayscale) 계산 후 초록색 비율만 높여 합성
                gray = (r + g + b) \ 3
                m_Pixels(i) = ((gray \ 2) * &H10000) Or (gray * &H100&) Or (gray \ 4)
            Next i
            
    End Select
    
    ' [4단계] 조작이 완료된 1차원 배열(m_Pixels)을 picClone 크기에 맞춰 화면에 즉시 출력!
    StretchDIBits PicClone.hDC, _
                  0, 0, PicClone.ScaleWidth, PicClone.ScaleHeight, _
                  0, 0, GBA_W, GBA_H, _
                  m_Pixels(0), m_BMI, DIB_RGB_COLORS, SRCCOPY
End Sub


Private Sub Form_KeyDown(KeyCode As Integer, Shift As Integer)
    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_KEYDOWN, KeyCode, MakeKeyLParam(KeyCode, False)
        KeyCode = 0
    End If
    
    ' F1 ~ F4 키를 누르면 1차원 배열 픽셀 조작 모드를 즉시 변경!
  '  Select Case KeyCode
  '      Case vbKeyF1: m_FilterMode = 0: Me.Caption = "필터: 0 (원본 배열)": Exit Sub
  '      Case vbKeyF2: m_FilterMode = 1: Me.Caption = "필터: 1 (CRT 스캔라인)": Exit Sub
  '      Case vbKeyF3: m_FilterMode = 2: Me.Caption = "필터: 2 (색상 반전 Xor)": Exit Sub
  '      Case vbKeyF4: m_FilterMode = 3: Me.Caption = "필터: 3 (초대 게임보이 녹색)": Exit Sub
  '  End Select
End Sub

Private Sub Form_KeyUp(KeyCode As Integer, Shift As Integer)
    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_KEYUP, KeyCode, MakeKeyLParam(KeyCode, True)
        KeyCode = 0
    End If
End Sub

Private Sub Form_Unload(Cancel As Integer)
    tmrCopy.Enabled = False
    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_CLOSE, 0, 0
        g_mgbaHwnd = 0
    End If
End Sub

