import 'dart:io';
import 'dart:math';

import 'package:fluent_ui/fluent_ui.dart' hide FluentIcons;
import 'package:fluentui_system_icons/fluentui_system_icons.dart';
import 'package:flutter_localized_locales/flutter_localized_locales.dart';
import 'package:get/get.dart';
import 'package:reboot_common/common.dart';
import 'package:reboot_launcher/l10n/reboot_localizations.dart';
import 'package:reboot_launcher/src/controller/dll_controller.dart';
import 'package:reboot_launcher/src/controller/settings_controller.dart';
import 'package:reboot_launcher/src/pager/page_type.dart';
import 'package:reboot_launcher/src/util/translations.dart';
import 'package:reboot_launcher/src/tile/file_setting_tile.dart';
import 'package:reboot_launcher/src/tile/setting_tile.dart';
import 'package:reboot_launcher/src/messenger/dialog.dart';
import 'package:reboot_launcher/src/pager/abstract_page.dart';
import 'package:url_launcher/url_launcher.dart';

final GlobalKey<TextFormBoxState> settingsConsoleDllInputKey = GlobalKey();
final GlobalKey<TextFormBoxState> settingsAuthDllInputKey = GlobalKey();
final GlobalKey<TextFormBoxState> settingsMemoryDllInputKey = GlobalKey();
final GlobalKey<TextFormBoxState> settingsGameServerDllInputKey = GlobalKey();

class SettingsPage extends AbstractPage {
  const SettingsPage({Key? key}) : super(key: key);

  @override
  String get name => translations.settingsName;

  @override
  String get iconAsset => "assets/images/settings.png";

  @override
  PageType get type => PageType.settings;

  @override
  bool hasButton(String? pageName) => false;

  @override
  AbstractPageState<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends AbstractPageState<SettingsPage> {
  final SettingsController _settingsController = Get.find<SettingsController>();
  final DllController _dllController = Get.find<DllController>();
  int? _downloadFromMirrorId;

  @override
  Widget? get button => null;

  // Project Stark client settings (read live by StarkClient.dll inside your game)
  double? _buildSens;
  double? _editSens;
  bool _resetOnRelease = false;
  final TextEditingController _editKey = TextEditingController(text: "G");
  final TextEditingController _resetKey = TextEditingController(text: "RightMouse");

  File get _starkSettingsFile => File("${Platform.environment['LOCALAPPDATA']}\\ProjectStark\\client_settings.ini");

  void _loadStarkSettings() {
    if (_buildSens != null) return;
    _buildSens = 1.0;
    _editSens = 1.0;
    try {
      for (final line in _starkSettingsFile.readAsLinesSync()) {
        final parts = line.split('=');
        if (parts.length != 2) continue;
        final value = double.tryParse(parts[1].trim());
        if (parts[0].trim() == 'ResetOnRelease') _resetOnRelease = parts[1].trim() == '1';
        if (parts[0].trim() == 'EditKey') _editKey.text = parts[1].trim();
        if (parts[0].trim() == 'ResetKey') _resetKey.text = parts[1].trim();
        if (value == null) continue;
        if (parts[0].trim() == 'BuildSensitivity') _buildSens = value;
        if (parts[0].trim() == 'EditSensitivity') _editSens = value;
      }
    } catch (_) {}
  }

  void _saveStarkSettings() {
    try {
      _starkSettingsFile.parent.createSync(recursive: true);
      _starkSettingsFile.writeAsStringSync(
          "[Client]\r\nBuildSensitivity=${_buildSens!.toStringAsFixed(2)}\r\nEditSensitivity=${_editSens!.toStringAsFixed(2)}\r\n"
          "ResetOnRelease=${_resetOnRelease ? 1 : 0}\r\nEditKey=${_editKey.text.trim()}\r\nResetKey=${_resetKey.text.trim()}\r\n");
    } catch (_) {}
  }

  Widget _starkSlider(double value, void Function(double) onChanged) => Row(
    children: [
      Expanded(
        child: Slider(
          min: 0.1,
          max: 2.0,
          divisions: 38,
          value: value.clamp(0.1, 2.0),
          onChanged: (newValue) {
            setState(() => onChanged(double.parse(newValue.toStringAsFixed(2))));
            _saveStarkSettings();
          },
        ),
      ),
      const SizedBox(width: 8.0),
      SizedBox(width: 44.0, child: Text("${(value * 100).round()}%")),
    ],
  );

  SettingTile get _starkClient {
    _loadStarkSettings();
    return SettingTile(
      icon: Icon(FluentIcons.cursor_24_regular),
      title: const Text("Project Stark"),
      subtitle: const Text("Build/edit sensitivity and reset on release (change them while you play)"),
      children: [
        SettingTile(
          icon: Icon(FluentIcons.building_24_regular),
          title: const Text("Build sensitivity"),
          subtitle: const Text("Mouse sensitivity while holding building pieces"),
          contentWidth: 260.0,
          content: _starkSlider(_buildSens!, (value) => _buildSens = value),
        ),
        SettingTile(
          icon: Icon(FluentIcons.edit_24_regular),
          title: const Text("Edit sensitivity"),
          subtitle: const Text("Mouse sensitivity while editing"),
          contentWidth: 260.0,
          content: _starkSlider(_editSens!, (value) => _editSens = value),
        ),
        SettingTile(
          icon: Icon(FluentIcons.arrow_reset_24_regular),
          title: const Text("Reset on release"),
          subtitle: const Text("While editing, letting go of your reset key confirms the edit for you"),
          content: ToggleSwitch(
            checked: _resetOnRelease,
            onChanged: (value) {
              setState(() => _resetOnRelease = value);
              _saveStarkSettings();
            },
          ),
        ),
        SettingTile(
          icon: Icon(FluentIcons.keyboard_24_regular),
          title: const Text("Edit key"),
          subtitle: const Text("Your Fortnite edit bind, e.g. G, F, Mouse4"),
          content: TextBox(controller: _editKey, onChanged: (_) => _saveStarkSettings()),
        ),
        SettingTile(
          icon: Icon(FluentIcons.keyboard_24_regular),
          title: const Text("Reset key"),
          subtitle: const Text("Your Fortnite reset-edit bind, e.g. RightMouse, R, Mouse5, WheelDown"),
          content: TextBox(controller: _resetKey, onChanged: (_) => _saveStarkSettings()),
        ),
      ],
    );
  }

  @override
  List<Widget> get settings => [
    _starkClient,
    _language,
    _theme,
    _internalFiles,
    _installationDirectory,
  ];

  SettingTile get _internalFiles => SettingTile(
    icon: Icon(
        FluentIcons.archive_settings_24_regular
    ),
    title: Text(translations.settingsClientName),
    subtitle: Text(translations.settingsClientDescription),
    children: [
      createFileSetting(
          key: settingsConsoleDllInputKey,
          title: translations.settingsClientConsoleName,
          description: translations.settingsClientConsoleDescription,
          controller: _dllController.unrealEngineConsoleDll,
          onReset: () async {
            final path = _dllController.getDefaultDllPath(GameDll.console);
            _dllController.unrealEngineConsoleDll.text = path;
            await _dllController.download(GameDll.console, path, force: true);
            settingsConsoleDllInputKey.currentState?.validate();
          }
      ),
      createFileSetting(
          key: settingsAuthDllInputKey,
          title: translations.settingsClientAuthName,
          description: translations.settingsClientAuthDescription,
          controller: _dllController.backendDll,
          onReset: () async {
            final path = _dllController.getDefaultDllPath(GameDll.auth);
            _dllController.backendDll.text = path;
            await _dllController.download(GameDll.auth, path, force: true);
            settingsAuthDllInputKey.currentState?.validate();
          }
      ),
      createFileSetting(
          key: settingsMemoryDllInputKey,
          title: translations.settingsClientMemoryName,
          description: translations.settingsClientMemoryDescription,
          controller: _dllController.memoryLeakDll,
          onReset: () async {
            final path = _dllController.getDefaultDllPath(GameDll.memoryLeak);
            _dllController.memoryLeakDll.text = path;
            await _dllController.download(GameDll.memoryLeak, path, force: true);
            settingsAuthDllInputKey.currentState?.validate();
          }
      ),
      _gameServer
    ],
  );

  SettingTile get _gameServer => SettingTile(
    icon: Icon(
        FluentIcons.document_24_regular
    ),
    title: Text(translations.settingsServerName),
    subtitle: Text(translations.settingsServerSubtitle),
    children: [
      _internalFilesServerType,
      _internalFilesServerSource,
      _internalFilesNewServerSource,
    ],
  );

  Widget get _internalFilesServerType => SettingTile(
      icon: Icon(
          FluentIcons.games_24_regular
      ),
      title: Text(translations.settingsServerTypeName),
      subtitle: Text(translations.settingsServerTypeDescription),
      contentWidth: SettingTile.kDefaultContentWidth + 30,
      content: Obx(() => DropDownButton(
          onOpen: () => inDialog = true,
          onClose: () => inDialog = false,
          leading: Text(_dllController.customGameServer.value ? translations.settingsServerTypeCustomName : translations.settingsServerTypeEmbeddedName),
          items: {
            false: translations.settingsServerTypeEmbeddedName,
            true: translations.settingsServerTypeCustomName
          }.entries.map((entry) => MenuFlyoutItem(
              text: Text(entry.value),
              onPressed: () {
                final oldValue = _dllController.customGameServer.value;
                if(oldValue == entry.key) {
                  return;
                }

                _dllController.customGameServer.value = entry.key;
                if(!entry.key) {
                  _dllController.updateGameServerDll(
                      force: true
                  );
                }
              }
          )).toList()
      ))
  );

  Widget get _internalFilesServerSource => Obx(() {
    if(!_dllController.customGameServer.value) {
      return SettingTile(
          icon: Icon(
              FluentIcons.globe_24_regular
          ),
          title: Text(translations.settingsServerOldMirrorName),
          subtitle: Text(translations.settingsServerMirrorDescription),
          contentWidth: SettingTile.kDefaultContentWidth + 30,
          content: Row(
            children: [
              Expanded(
                child: TextFormBox(
                    placeholder:  translations.settingsServerMirrorPlaceholder,
                    controller: _dllController.beforeS20Mirror,
                    onChanged: _scheduleMirrorDownload
                ),
              ),
              const SizedBox(width: 8.0),
              Button(
                  style: ButtonStyle(
                      padding: WidgetStateProperty.all(EdgeInsets.zero)
                  ),
                  onPressed: () => _dllController.updateGameServerDll(force: true),
                  child: SizedBox.square(
                    dimension: 30,
                    child: Icon(
                        FluentIcons.arrow_download_24_regular
                    ),
                  )
              ),
              const SizedBox(width: 8.0),
              Button(
                  style: ButtonStyle(
                      padding: WidgetStateProperty.all(EdgeInsets.zero)
                  ),
                  onPressed: () {
                    _dllController.beforeS20Mirror.text = kRebootBelowS20DownloadUrl;
                    _dllController.updateGameServerDll(force: true);
                  },
                  child: SizedBox.square(
                    dimension: 30,
                    child: Icon(
                        FluentIcons.arrow_reset_24_regular
                    ),
                  )
              )
            ],
          )
      );
    }else {
      return createFileSetting(
          key: settingsGameServerDllInputKey,
          title: translations.settingsOldServerFileName,
          description: translations.settingsServerFileDescription,
          controller: _dllController.customGameServerDll,
          onReset: () async {
            final path = _dllController.getDefaultDllPath(GameDll.gameServer);
            _dllController.customGameServerDll.text = path;
            await _dllController.download(GameDll.gameServer, path);
            settingsGameServerDllInputKey.currentState?.validate();
          }
      );
    }
  });

  void _scheduleMirrorDownload(String value) async {
    if(_downloadFromMirrorId != null) {
      return;
    }

    if(Uri.tryParse(value) == null) {
      return;
    }

    final id = Random.secure().nextInt(1000000);
    _downloadFromMirrorId = id;
    await Future.delayed(const Duration(seconds: 2));
    if(_downloadFromMirrorId == id) {
      await _dllController.updateGameServerDll(force: true);
    }
    _downloadFromMirrorId = null;
  }

  Widget get _internalFilesNewServerSource => Obx(() {
    if(!_dllController.customGameServer.value) {
      return SettingTile(
          icon: Icon(
              FluentIcons.globe_24_regular
          ),
          title: Text(translations.settingsServerNewMirrorName),
          subtitle: Text(translations.settingsServerMirrorDescription),
          contentWidth: SettingTile.kDefaultContentWidth + 30,
          content: Row(
            children: [
              Expanded(
                child: TextFormBox(
                    placeholder: translations.settingsServerMirrorPlaceholder,
                    controller: _dllController.aboveS20Mirror,
                    onChanged: _scheduleMirrorDownload
                ),
              ),
              const SizedBox(width: 8.0),
              Button(
                  style: ButtonStyle(
                      padding: WidgetStateProperty.all(EdgeInsets.zero)
                  ),
                  onPressed: () => _dllController.updateGameServerDll(force: true),
                  child: SizedBox.square(
                    dimension: 30,
                    child: Icon(
                        FluentIcons.arrow_download_24_regular
                    ),
                  )
              ),
              const SizedBox(width: 8.0),
              Button(
                  style: ButtonStyle(
                      padding: WidgetStateProperty.all(EdgeInsets.zero)
                  ),
                  onPressed: () {
                    _dllController.aboveS20Mirror.text = kRebootBelowS20DownloadUrl;
                    _dllController.updateGameServerDll(force: true);
                  },
                  child: SizedBox.square(
                    dimension: 30,
                    child: Icon(
                        FluentIcons.arrow_reset_24_regular
                    ),
                  )
              )
            ],
          )
      );
    }else {
      return const SizedBox();
    }
  });

  SettingTile get _language => SettingTile(
      icon: Icon(
          FluentIcons.local_language_24_regular
      ),
      title: Text(translations.settingsUtilsLanguageName),
      subtitle: Text(translations.settingsUtilsLanguageDescription),
      content: Obx(() => DropDownButton(
          onOpen: () => inDialog = true,
          onClose: () => inDialog = false,
          leading: Text(_getLocaleName(_settingsController.language.value)),
          items: AppLocalizations.supportedLocales.map((locale) => MenuFlyoutItem(
              text: Text(_getLocaleName(locale.languageCode)),
              onPressed: () => _settingsController.language.value = locale.languageCode
          )).toList()
      ))
  );

  String _getLocaleName(String locale) {
    var result = LocaleNames.of(context)!.nameOf(locale);
    if(result != null) {
      return "${result.substring(0, 1).toUpperCase()}${result.substring(1).toLowerCase()}";
    }

    return locale;
  }

  SettingTile get _theme => SettingTile(
      icon: Icon(
          FluentIcons.dark_theme_24_regular
      ),
      title: Text(translations.settingsUtilsThemeName),
      subtitle: Text(translations.settingsUtilsThemeDescription),
      content: Obx(() => DropDownButton(
          onOpen: () => inDialog = true,
          onClose: () => inDialog = false,
          leading: Text(_settingsController.themeMode.value.title),
          items: ThemeMode.values.map((themeMode) => MenuFlyoutItem(
              text: Text(themeMode.title),
              onPressed: () => _settingsController.themeMode.value = themeMode
          )).toList()
      ))
  );

  SettingTile get _installationDirectory => SettingTile(
      icon: Icon(
          FluentIcons.folder_24_regular
      ),
      title: Text(translations.settingsUtilsInstallationDirectoryName),
      subtitle: Text(translations.settingsUtilsInstallationDirectorySubtitle),
      content: Button(
        onPressed: () => launchUrl(installationDirectory.uri),
        child: Text(translations.settingsUtilsInstallationDirectoryContent),
      )
  );
}

extension _ThemeModeExtension on ThemeMode {
  String get title {
    switch(this) {
      case ThemeMode.system:
        return translations.system;
      case ThemeMode.dark:
        return translations.dark;
      case ThemeMode.light:
        return translations.light;
    }
  }
}