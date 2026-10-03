"""Published explicit-template compatibility APIs remain source driven."""
import json
import unittest
import fatcat_metadata as fatcat


class ExplicitTemplateCompatibilityTests(unittest.TestCase):
    def test_printer_template_policy_is_independent_of_brand_and_name(self):
        project = {'printer_settings_id': 'old-device', 'default_print_profile': 'user-process',
                   'filament_settings_id': ['user-PLA'], 'nozzle_temperature': ['219'],
                   'filament_flow_ratio': ['0.97'], 'unknown': {'keep': True}}
        machine = {'printer_settings_id': 'native-device', 'nozzle_diameter': ['0.4'],
                   'default_print_profile': 'native-process', 'filament_settings_id': ['native-PLA']}
        common = {'slicer_id': 'OrcaSlicer', 'project_template_kind': 'printer',
                  'legacy_nozzle_size': '0.4', 'registry_nozzle_size': '0.4'}
        results = []
        for brand, name in [('Snapmaker', 'Snapmaker U1'), ('other', 'other-device')]:
            options = {**common, 'device_brand': brand, 'device_display_name': name}
            results.append(json.loads(fatcat.assemble_project_template(json.dumps(project), json.dumps(machine), json.dumps(options))))
        self.assertEqual(results[0], results[1])
        for key in ('default_print_profile', 'filament_settings_id', 'nozzle_temperature', 'filament_flow_ratio', 'unknown'):
            self.assertEqual(results[0][key], project[key])
        self.assertEqual(results[0]['printer_settings_id'], 'native-device')

    def test_absent_registry_preserves_explicit_source_fields(self):
        project = {'nozzle_temperature': ['231'], 'unknown': [1, 'keep']}
        self.assertEqual(json.loads(fatcat.assemble_project_template(json.dumps(project), None, '{}')), project)

    def test_none_kind_uses_explicit_machine_without_material_completion(self):
        machine = {'printer_model': 'explicit-device', 'nozzle_diameter': ['0.6']}
        result = json.loads(fatcat.assemble_project_template('{"old": true}', json.dumps(machine), '{"project_template_kind":"none"}'))
        self.assertEqual(result, machine)
        self.assertNotIn('nozzle_temperature', result)

    def test_minimal_kind_remains_explicit_historical_compatibility(self):
        result = json.loads(fatcat.assemble_project_template('{}', None, '{"project_template_kind":"minimal"}'))
        self.assertEqual(result['filament_settings_id'], ['Generic PLA'] * 8)
        self.assertEqual(result['nozzle_temperature'], ['220'] * 8)
        self.assertNotIn('printer_model', result)

    def test_registry_passthrough_keeps_user_settings_without_protected_options(self):
        settings = {'nozzle_temperature': ['231'], 'unknown': {'keep': True}}
        result = json.loads(fatcat.assemble_machine_registry_template(json.dumps(settings), '{}'))
        self.assertEqual(result['settings'], settings)
        self.assertIsNone(result['application_metadata'])
