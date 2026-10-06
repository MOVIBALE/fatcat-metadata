"""Sparse selections retain actual source settings and global material identity."""
import json
import unittest

import fatcat_metadata as fatcat

TARGETS = (
    ('BambuStudio', '02.08.02.61', 'bambu-lab:a1', 'plate:textured-pei'),
    ('OrcaSlicer', '2.4.2', 'snapmaker:u1', 'plate:textured-pei'),
    ('QIDIStudio', '02.07.02.60', 'qidi:q2', 'plate:cool'),
    ('ElegooSlicer', '1.5.3.5', 'elegoo:centauri', 'plate:cool'),
    ('AnycubicSlicerNext', '2.0.0.3', 'anycubic:kobra-2-pro', 'plate:cool'),
    ('FlashStudio', '1.7.18', 'flashforge:ad5x', 'plate:textured-pei'),
    ('SnapmakerOrca', '2.4.0', 'snapmaker:u1', 'plate:textured-pei'),
)


def native_request(target, materials):
    slicer, version, machine, plate = target
    return {'project_source': 'fatcat_native', 'slicer_id': slicer,
            'application_version': version, 'machine_uid': machine,
            'nozzle_uid': 'nozzle:0.4mm', 'build_plate_uid': plate,
            'filament_slot_mode': 'compact', 'source_materials': materials}


def native_project(target, materials):
    result = json.loads(fatcat.compose_project_settings(json.dumps(native_request(target, materials))))
    return json.loads(result['project_settings_json'])


def slot(project, global_id, name, colour):
    return {'source_slot_id': global_id, 'source_slot_index': 0, 'slot_name': name,
            'preview_color': colour, 'material_id': project['filament_settings_id'][0]}


class SparseMergeTests(unittest.TestCase):
    def test_absent_palette_rows_use_native_materials_and_keep_source_values(self):
        palette = [{'name': f'Material {i}', 'colour': f'#{i + 1:06X}'} for i in range(4)]
        for target in TARGETS:
            with self.subTest(target=target[0]):
                project = native_project(target, [palette[2]])
                project.update({'nozzle_temperature': ['219'], 'filament_flow_ratio': ['0.97'],
                                'vendor_unknown': {'preserve': [1, 'yes']}, 'version': 'custom-saved-version',
                                'default_print_profile': 'custom-process',
                                'inherits_group': ['custom-process', 'custom-material', 'custom-printer']})
                if target[0] == 'SnapmakerOrca':
                    project['small_area_infill_flow_compensation_model'] = ['user-flow-model']
                request = native_request(target, palette)
                request.pop('project_source')
                request.pop('filament_slot_mode', None)
                request.update({'hardware_mode': 'preserve_source', 'material_mode': 'preserve_template',
                                'consumer_type': 'native_preset',
                                'merge_sources': [{'source_id': 'piece', 'slots': [slot(project, 2, palette[2]['name'], palette[2]['colour'])]}]})
                for mode in ('preserve_source', 'target_binding'):
                    with self.subTest(hardware_mode=mode):
                        request['hardware_mode'] = mode
                        result = json.loads(fatcat.compose_project_settings(json.dumps(project), json.dumps(request)))
                        final = json.loads(result['project_settings_json'])
                        self.assertEqual(final['filament_colour'], [m['colour'] for m in palette])
                        self.assertEqual(final['nozzle_temperature'][2], '219')
                        self.assertEqual(final['filament_flow_ratio'][2], '0.97')
                        if target[0] == 'SnapmakerOrca':
                            self.assertEqual(final['small_area_infill_flow_compensation_model'], ['user-flow-model'])
                        for key in ('vendor_unknown', 'version', 'default_print_profile', 'inherits_group'):
                            self.assertEqual(final[key], project[key], key)
                        self.assertEqual(result['source_slot_mappings'][0]['slots'][0]['output_slot_index'], 2)

    def test_disjoint_complete_palette_retains_both_source_materials(self):
        target = TARGETS[1]
        palette = [{'name': 'First', 'colour': '#112233'}, {'name': 'Second', 'colour': '#445566'}]
        projects = [native_project(target, [material]) for material in palette]
        for project, temperature, diagonal in zip(projects, ['211', '233'], ['11', '17']):
            project['nozzle_temperature'] = [temperature]
            project['flush_volumes_matrix'] = [diagonal]
        request = native_request(target, palette)
        request.pop('project_source')
        request.pop('filament_slot_mode', None)
        request.update({'hardware_mode': 'preserve_source', 'material_mode': 'preserve_template',
                        'merge_sources': [{'source_id': str(i), 'slots': [slot(project, i, palette[i]['name'], palette[i]['colour'])],
                                           **({'project_settings': project} if i else {})}
                                          for i, project in enumerate(projects)]})
        for hardware_mode in ('preserve_source', 'target_binding'):
            with self.subTest(hardware_mode=hardware_mode):
                request['hardware_mode'] = hardware_mode
                result = json.loads(fatcat.compose_project_settings(json.dumps(projects[0]), json.dumps(request)))
                final = json.loads(result['project_settings_json'])
                self.assertEqual(final['nozzle_temperature'], ['211', '233'])
                self.assertEqual(len(final['flush_volumes_matrix']), 4)
                self.assertEqual(final['flush_volumes_matrix'][0], '11')
                self.assertEqual(final['flush_volumes_matrix'][3], '17')
                defaults = native_project(target, palette)['flush_volumes_matrix']
                self.assertEqual(final['flush_volumes_matrix'], ['11', defaults[1], defaults[2], '17'])
                self.assertEqual(len(result['source_slot_mappings']), 2)

    def test_native_transition_defaults_do_not_hide_source_conflicts(self):
        target = TARGETS[1]
        palette = [{'name': 'First', 'colour': '#112233'}, {'name': 'Second', 'colour': '#445566'}]
        first = native_project(target, [palette[0]])
        first['flush_volumes_matrix'] = ['11']
        other = {**first, 'flush_volumes_matrix': ['17']}
        request = native_request(target, palette)
        request.pop('project_source')
        request.pop('filament_slot_mode', None)
        request.update({'material_mode': 'preserve_template', 'merge_sources': [
            {'source_id': 'first', 'slots': [slot(first, 0, 'First', '#112233')]},
            {'source_id': 'other', 'slots': [slot(other, 0, 'First', '#112233')], 'project_settings': other},
        ]})
        for mode in ('preserve_source', 'target_binding'):
            with self.subTest(hardware_mode=mode):
                request['hardware_mode'] = mode
                with self.assertRaisesRegex(ValueError, 'conflict for flush_volumes_matrix'):
                    fatcat.compose_project_settings(json.dumps(first), json.dumps(request))

    def test_transition_defaults_require_source_hardware_in_both_modes(self):
        target = TARGETS[1]
        project = native_project(target, [{'name': 'First', 'colour': '#112233'}])
        defaults = {**project, 'printer_model': 'different-device'}
        request = {'slicer_id': target[0], 'application_version': target[1],
                   'machine_uid': target[2], 'nozzle_uid': 'nozzle:0.4mm',
                   'build_plate_uid': target[3], 'material_mode': 'preserve_template',
                   'merge_sources': [{'source_id': 'piece', 'slots': [slot(project, 0, 'First', '#112233')]}],
                   'merge_default_project': defaults}
        for mode in ('preserve_source', 'target_binding'):
            with self.subTest(hardware_mode=mode):
                request['hardware_mode'] = mode
                with self.assertRaisesRegex(ValueError, 'machine model does not match'):
                    fatcat.compose_project_settings(json.dumps(project), json.dumps(request))

    def test_native_material_selection_preserves_explicit_u1_process_fields(self):
        project = native_project(TARGETS[-1], [{'name': 'PLA', 'colour': '#123456'}])
        project.update({'small_area_infill_flow_compensation_model': ['user-model'],
                        'version': 'custom-version', 'default_print_profile': 'custom-process',
                        'inherits_group': ['process', 'material', 'printer']})
        request = {'slicer_id': 'SnapmakerOrca', 'application_version': '2.4.0',
                   'machine_uid': 'snapmaker:u1', 'nozzle_uid': 'nozzle:0.4mm',
                   'build_plate_uid': 'plate:textured-pei', 'material_uid': 'material:pla',
                   'material_mode': 'target_native_preset', 'filament_colour': ['#123456']}
        result = json.loads(fatcat.compose_project_settings(json.dumps(project), json.dumps(request)))
        final = json.loads(result['project_settings_json'])
        for key in ('small_area_infill_flow_compensation_model', 'version',
                    'default_print_profile', 'inherits_group'):
            self.assertEqual(final[key], project[key], key)

    def test_missing_material_completion_rejects_different_hardware(self):
        palette = [{'name': 'First', 'colour': '#112233'}, {'name': 'Second', 'colour': '#445566'}]
        project = native_project(TARGETS[0], [palette[0]])
        request = native_request(TARGETS[0], palette)
        request.pop('project_source')
        request.pop('filament_slot_mode', None)
        request.update({'hardware_mode': 'preserve_source', 'machine_uid': 'bambu-lab:a1-mini',
                        'merge_sources': [{'source_id': 'piece', 'slots': [slot(project, 0, 'First', '#112233')]}]})
        with self.assertRaisesRegex(ValueError, 'hardware differs'):
            fatcat.compose_project_settings(json.dumps(project), json.dumps(request))


if __name__ == '__main__':
    unittest.main()
