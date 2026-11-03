# Dynamic Settings

本モジュールは、nemu-link用に開発した、Devicetreeで宣言した設定をZMK Studio RPCから読み書きするためのZephyr moduleです。ビルド時に設定schemaを生成し、nemu-link Studioで表示するWidgetも指定できます。設定追加によるROM/RAMのオーバーヘッドを抑える設計です。

本moduleはnemu-link用のZMK構成を前提とします。

## 導入（最小構成）

### west.yml

この例は既存の`west.yml`へ追記します。`sekigon-gonnoc` remoteが既にある場合は重ねて定義しません。nemu-link構成では、互換性のあるZMK forkと`zmk-feature-nemu-link`もmanifestに必要です。

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk-feature-dynamic-settings
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

manifestを更新した後、`west update zmk-feature-dynamic-settings`を実行します。

### .conf

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_RPC=y
CONFIG_ZMK_DYNAMIC_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

`CONFIG_ZMK_DYNAMIC_SETTINGS`がZephyr Settingsを有効にします。`CONFIG_SETTINGS_NVS`はNVS backendを使う場合の設定です。別のSettings backendを使う場合は、対象boardのbackendを有効にしてください。

### overlay

Dynamic Settings単体ではoverlay nodeを必要としません。設定を公開する側が`sekigon,dynamic-settings-group`と`sekigon,dynamic-setting`を宣言します。feature moduleは必要なgroupを生成するため、各feature READMEのoverlay例を使ってください。独自の設定groupを作る場合は、以下の開発者向け説明を参照します。

## 追加設定

必要なfeature moduleだけを同じmanifestに追加します。各featureはDynamic Settingsを自動取得しません。

```yaml
    - name: zmk-feature-dynamic-combo
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-encoder
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-macro
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-tap-dance
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-input-processor
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-ble-profile
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

各entryを既存manifestの`manifest.projects`へ追加します。`CONFIG_SETTINGS_NVS`の代わりに別のZephyr Settings backendも使えます。NVSを使う場合は、board側で保存領域を設定します。大きいRPC requestを送るfirmwareでは`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE`を増やせます。このbufferは静的RAMを使います。

## 開発者向けのドキュメント


本モジュールを使用して、動的に設定を変更できるモジュールを設計する際に必要な情報です。

### 実デバイスのsettings groupを宣言する

実際に設定を適用する側では`sekigon,dynamic-settings-group`を使います。groupの識別には`module-name`、`group-name`、`instance-name`を指定します。この3つとsetting名から安定したIDが生成され、生成headerに`DYNAMIC_GROUP_ID_*`と`DYNAMIC_SETTING_ID_*`のmacroが出力されます。

多くの`zmk-feature-dynamic-*`モジュールでは利用者がこれらの設定をすべて書かなくてもいいように、専用のマクロやスクリプトを用意することを推奨します。

```dts
/ {
    pointer_sensor_settings: pointer_sensor_settings {
        compatible = "sekigon,dynamic-settings-group";
        module-name = "pointer";
        group-name = "sensor";
        instance-name = "right";
        label = "Pointer sensor";
        scope = "owner";
        format-version = <1>;
        target-role = "keyboard-right";
        actual-device = <&pointing_device>;

        cpi: cpi {
            compatible = "sekigon,dynamic-setting";
            setting-name = "cpi";
            label = "CPI";
            value-type = "u16";
            default-property = "res-cpi";
            minimum = <608>;
            maximum = <4826>;
            step = <38>;
            unit = "CPI";
            widget = "slider";
            help = "Pointer sensor resolution";
            persistent;
            on-change = "paw3222_dynamic_cpi_changed";
        };
    };
};
```

groupには次を指定します。

- `scope`: `local`、`shared`、`owner`のいずれか。保存範囲と所有関係を表します。
- `kconfig-condition`: 必要なgroupだけschemaに含める場合にKconfig symbol名を指定します。有効でないsymbolを指定したgroupはschema生成時に除外されます。
- `format-version`: group blobの格納形式を変更するときに増やします。scalar settingの型や保存互換性の変更はsetting IDで個別に扱います。
- `target-role`: 実デバイス側のgroupを識別するroleです。
- `actual-device`: 設定を適用するZephyr deviceへのphandleです。
- `default-provider-context-free`: deviceに依存しない`default-provider`を使うgroupで指定します。この場合は`actual-device`を省略でき、providerには`NULL` contextが渡されます。`default-property`やdevice向けcallbackを使うsettingには`actual-device`が必要です。

settingの`value-type`は`bool`、`u8`、`u16`、`u32`、`s8`、`s16`、`s32`、`enum`、`text`から選びます。数値設定では`minimum`、`maximum`、`step`、enumでは`enum-values`を指定するとStudio UIのスキーマに含まれます。値の範囲チェックはUI側で行う設計です。ファームウェアdescriptorには範囲を重複して持たせません。

`text`はUTF-8のtextbox設定です。`text-length`で最大byte数（1..256）を指定し、必要なら`default-string`でdefault値を指定します。schema generatorがこの1つの宣言を、既存のscalar保存・通信経路で扱える複数の`u32` settingへ展開します。Studioにはtextboxとしてまとまって表示されます。

値は既定で永続化対象です。`read-only`はStudioからの書き込みを禁止し、`reboot-required`は再起動後に適用する設定を表します。`volatile`をread-only設定に付けると、保存せずにruntime read providerから値を取得できます。groupの`runtime-read-provider`で`dynamic_setting_runtime_read_fn`型の関数を指定すると、そのgroupのread-only項目の読み出し時に呼び出されます。表示には`label`、`help`、`unit`、`widget`を指定できます。

### 初期値をデバイス設定から取得する

実デバイス側のsettingには、次のどちらかで初期値を指定します。

- `default-property`: `actual-device`が指すDevicetree nodeのproperty名。たとえば`default-property = "res-cpi"`なら、`actual-device`の`res-cpi`値を使います。
- `default-bool`、`default-int`、`default-enum`: overlay側で値を直接指定する方法。

`default-property`を使うと、モジュールやdriverの既存Devicetree設定とStudioの初期値を共有できます。bool propertyは未指定ならfalseとして扱います。enumではproperty名から自動変換できないため、`default-enum`でenumの文字列値を指定します。直接指定するdefaultと`default-property`は併用できません。

通常の定数やDevicetree propertyから作れない初期値だけ、groupの`default-provider`でC関数を指定します。provider関数の型は`dynamic_setting_default_fn`です。各settingごとにgetterを書く必要はありません。providerにはsetting descriptorが渡るため、descriptor IDに応じた値を返します。

### 値変更をdriverへ反映する

即時反映が必要なsettingには`on-change`でC関数名を指定します。関数は次の型に合わせて実装します。

```c
#include <zmk/dynamic_settings/store.h>

int paw3222_dynamic_cpi_changed(const struct dynamic_setting_descriptor *descriptor,
                             const struct dynamic_setting_value *new_value,
                             enum dynamic_setting_change_reason reason,
                             const void *context) {
    (void)descriptor;
    (void)reason;
    const struct device *device = context;
    uint16_t cpi = sys_get_le16(new_value->data);
    return paw3222_set_cpi(device, cpi);
}
```

実際の関数では`<zephyr/sys/byteorder.h>`もincludeしてください。`context`には`actual-device`のdevice pointerが渡ります。settingの型に合わせて`new_value->data`を読み、driverの公開APIを呼び出します。boolとenumは1 byte、整数はlittle-endianです。callbackはgroup内でcallbackを持つsettingだけに登録されます。

callbackは起動時の保存値ロード、StudioからのTEMP書き込み、reset、discardなどで呼び出されます。`reason`で呼び出し理由を判定できます。`reboot-required`のsettingは再起動後にdevice initから適用し、稼働中のTEMP変更では即時callbackを呼びません。設定がdriver初期化より前に必要な場合は`dynamic_settings_apply_init_required(device)`をdriverの初期化処理から呼びます。

設定値を`actual-device`のruntime data構造体の同型フィールドへコピーするだけなら、settingごとの`on-change`もC適用関数も不要です。groupにruntime構造体のheader/typeを指定し、各settingに`runtime-field`でメンバーを宣言します。必要なら`runtime-lock-field`でコピー時のspinlockも指定できます。

```dts
runtime-struct-header = "zmk/example_runtime.h";
runtime-struct-type = "example_runtime_params";
runtime-lock-field = "lock";

speed: speed {
    compatible = "sekigon,dynamic-setting";
    setting-name = "speed";
    value-type = "u16";
    runtime-field = "speed";
    /* UI metadata and default as usual */
};
```

GeneratorはC member offsetとwidthをdescriptorへ出力し、fieldの幅とsettingのwire型が一致しない場合はbuildを失敗させます。runtime構造体は`actual-device`の`data`が指す構造体です。`runtime-field`を指定した設定値はruntime構造体を正本として読み書きし、Storeには変更状態だけを記録します。保存値は起動時にruntime fieldへ反映し、Store内に値の複製を残しません。Storeの保存処理もruntime fieldから値を読みます。

派生状態の更新や追加検証が必要なgroupは、一つの`runtime-apply-provider`を指定できます。これは共通コピーの後に呼び出され、失敗時は対象fieldを元に戻します。`runtime-field`とsettingごとの`on-change`は併用できません。センサーAPI呼び出しなど単純コピーでない設定は、引き続きsettingごとの`on-change`を使います。

### split側にproxyを宣言する

実デバイスが別MCUにある場合、そのfirmwareでは`sekigon,dynamic-settings-proxy-group`を宣言し、実デバイス側と同じ3つの名前、scope、format-version、setting名と型を記述します。

```dts
pointer_sensor_settings: pointer_sensor_settings {
    compatible = "sekigon,dynamic-settings-proxy-group";
    module-name = "pointer";
    group-name = "sensor";
    instance-name = "right";
    label = "Pointer sensor";
    scope = "owner";
    format-version = <1>;
    target-id = "keyboard-right";

    cpi: cpi {
        compatible = "sekigon,dynamic-setting";
        setting-name = "cpi";
        label = "CPI";
        value-type = "u16";
        default-int = <608>;
        minimum = <608>;
        maximum = <4826>;
        step = <38>;
        unit = "CPI";
        widget = "slider";
        persistent;
    };
};
```

proxyでは`target-id`を指定し、`actual-device`、`default-property`、`default-provider`、`on-change`は指定しません。現状、proxy descriptorは宣言とスキーマ生成用です。split越しの設定転送は別途relay実装が必要です。

### buildと生成物を確認する

West manifestを更新した後、対象firmwareで通常のZMK/Zephyr buildを実行します。build wrapperを使う場合は、そのwrapperが`west.yml`を使って取得したmoduleを含むworkspaceを参照していることを確認してください。

Devicetreeからbuildごとの`settings_schema_generated.h`と`settings_schema_generated.c`が生成されます。生成headerには設定ID、C fileにはgroup/setting descriptor、callback table、圧縮されたcanonical CBORスキーマが入ります。生成物はbuild directory内に作られるため、moduleへcommitする必要はありません。設定数ごとにdescriptorのROMは必要ですが、labelやrangeなどUI向けmetadataは圧縮スキーマに集約され、callback pointerもcallbackがあるsettingだけに保存されます。
