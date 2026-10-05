<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Sigmoid Neuron Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# Sigmoid Neuron Translator — Projet et feuille de route

`neuron-translator` est l'**unité de calcul** de la pile GNU AI pour
GNU/Hurd : un réseau de neurones sigmoïdes feedforward exposé comme un
translator Hurd (`trivfs`), piloté uniquement par `read`/`write`
POSIX. Il est conçu pour de **très grands réseaux** (100 000 neurones
et plus) avec une empreinte mémoire minimale (stockage `float32`,
allocation contiguë) et une consommation CPU maîtrisée (zéro
allocation dans le chemin chaud). Il ne fait ni orchestration, ni
persistance, ni transport : il transforme une topologie, des poids et
un vecteur d'entrée en vecteur de sortie, et rien d'autre.

Licence : GPLv3 ou version ultérieure. Langage : C23, POSIX.1-2008,
interfaces Hurd (`trivfs`).

---

## 1. Rôle et positionnement

L'esprit Hurd est respecté : chaque responsabilité reste dans un
translator dédié, le calcul ne fait que **calculer**.

| Composant | Responsabilité | Lien |
|---|---|---|
| `neuron-translator` | unité de calcul : réseau sigmoïde feedforward, piloté par POSIX | ce dépôt |
| `orchestrator-translator` | coordination : scheduler, supervisor, evaluator, aggregator ; monte N instances et agrège leurs votes | gnu-ai/orchestrator-translator |
| `inference-translator` | interface de dialogue : prompts, éditeur, requêtes structurées, mode distant SSH | gnu-ai/inference-translator |
| `httpfs-translator` | transport pur HTTP → système de fichiers | gnu-ai/httpfs-translator |
| `data-base-translator` | persistance PostgreSQL (données d'entraînement, exécutions, résultats) | gnu-ai/data-base-translator |
| `mistral-vm-debian-hurd` | sandbox CI QEMU headless : construire et tester sur GNU/Hurd réel | gnu-ai/mistral-vm-debian-hurd |

### Ce que ce translator garantit aux autres

- **Un contrat de commandes gelé** : statut, topologie, entrées,
  `save`, `load`, `reset`. L'orchestrateur monte autant d'instances
  qu'il veut par `settrans` et les pilote sans jamais lier ce binaire
  ni connaître son implémentation.
- **Reproductibilité** : un montage frais porte des poids
  pseudo-aléatoires déterministes (formule documentée dans le
  README) ; mêmes topologie et mêmes poids donnent toujours la même
  sortie — la pile reste rejouable.
- **Remplaçabilité** : tout backend de calcul qui respecte le même
  contrat de commandes peut être monté à la place de celui-ci. Un
  jour, un backend GPU entrerait comme un composant monté de plus.

### Ce qu'il ne fait pas

- **Aucun entraînement** : rien dans ce dépôt ne modifie un poids
  après l'initialisation. Les poids significatifs viennent de
  fichiers `.nn` chargés par `load`, produits par un entraîneur
  externe — le format `.nn` (v2) est le contrat que cet entraîneur
  implémente. L'entraînement est une préoccupation séparée de la
  pile, pas de ce translator.
- Pas d'orchestration, pas de requête réseau, pas de persistance :
  pas de vote, pas de stockage, pas d'acquisition.

## 2. Fonctionnalités principales

- **Efficacité mémoire** : stockage `float32` (4 octets au lieu de
  8), bloc unique contigu pour tout le réseau (arena allocator),
  structures compactes, alignement 16 octets compatible SIMD,
  croissance linéaire avec la taille du réseau.
- **Efficacité CPU** : accès mémoire séquentiels (cache-friendly),
  branchements minimaux et prévisibles dans les boucles chaudes,
  fonctions critiques inlinées, zéro allocation à l'exécution
  après l'initialisation.
- **Pilotage POSIX complet** :
  - lecture du statut du réseau et de la sortie courante par un
    simple `read` sur le point de montage ;
  - configuration de la topologie (`echo '3,5,2' > /llm`, couches
    séparées par virgules ou espaces) ;
  - fourniture des entrées (le nombre de valeurs doit correspondre
    exactement à la taille de la couche d'entrée) ;
  - `save <fichier>` / `load <fichier>` : sérialisation du réseau
    (topologie + poids) au format `.nn` ;
  - `reset` : ré-amorçage de l'état volatile (voltages, compteurs)
    sans toucher aux poids.
- **Modèle** : activation sigmoïde f(x) = 1 / (1 + exp(-x)), réseau
  feedforward entrée → couches cachées → sortie.

## 3. Architecture et flux de données

Le translator est monté par `settrans` sur un nœud du système de
fichiers (les exemples du README utilisent `/llm`). Tout passe par ce
nœud : il n'y a pas de client dédié, pas de socket, pas de
bibliothèque partagée.

### Flux nominal d'une inférence

1. `settrans /llm …` : montage, `network_init()` alloue l'arena
   unique et amorce les poids (valeur pseudo-aléatoire déterministe
   dans [-0.2, 0.2] — placeholder de câblage, voir *Scope* dans le
   README).
2. `write` d'une topologie : réallocation de l'arena à la nouvelle
   géométrie.
3. `write` des entrées : forward pass (`network_forward()`) sans
   allocation ; la sortie devient lisible par `read`.
4. `load model.nn` (optionnel) : remplace les poids par ceux d'un
   réseau entraîné — sans ce chargement, les sorties sont du bruit
   reproductible : la plomberie fonctionne, les nombres ne
   signifient rien.
5. `save result.nn` : rediffusion du réseau courant vers un autre
   montage, même sur une autre machine de la pile.

Dans la pile complète, l'orchestrateur répète ce schéma sur
`/llm1..N` et agrège les votes ; data-base-translator archive ce qui
a été calculé, rejouable à l'identique plus tard.

## 4. Décisions de conception

### Un moteur d'inférence, pas un entraîneur

Un montage frais vote sur du bruit tant qu'aucun fichier `.nn`
n'est chargé — et c'est assumé : ce qui est éprouvé, c'est la
plomberie (isolation de pannes, auditabilité, composition,
rejouabilité des mêmes nombres). L'entraînement vit ailleurs dans
la pile ; ce dépôt ne spécifie que le contrat de format par lequel
des poids entraînés entrent.

### `float32` et arena contiguë

Un seul bloc mémoire pour tout le réseau, aligné 16 octets :
localité cache, SIMD possible, zéro fragmentation, croissance
linéaire. Le forward pass n'alloue rien : ce qui est mesuré au
montage est ce qui est consommé à l'exécution.

### Format `.nn` v2 : contrat gelé

`NET_FILE_VERSION` vaut 2 ; la topologie sérialisée a changé avec
la suppression des paramètres spiking et les fichiers v1 sont
rejetés par `network_load()`. Le format est documenté champ par
champ dans le README : c'est le seul point d'entrée d'un entraîneur
externe, il ne changera pas silencieusement.

### Paramètres spiking supprimés

Les champs `threshold`, `leak_rate` et `refractory_length` étaient
positionnés et affichés mais jamais lus : `network_forward()` a
toujours été un feedforward sigmoïde pur. Ils ont été retirés de
`NetworkTopology`, de `network_init()` et de la vue de statut
(voir `CHANGES.md`, 2026-10) : pas de code mort, pas de promesse de
README non tenue.

### Multitâche et multi-utilisateurs

Le translator sert plusieurs appelants et plusieurs utilisateurs
simultanément : écritures sérialisées proprement, état de lecture
propre à chaque lecteur, aucun état global non protégé.

## 5. Phases

### Phase 0 — Spécification et contrats (terminée)

Périmètre gelé : commandes POSIX, format `.nn`, garantie
zéro-allocation. Le README fait foi.

### Phase 1 — MVP : translator + forward pass (terminée)

`trivfs` monté par `settrans`, arena `float32`, forward pass
optimisé, statut lisible, entrées/sorties par `write`/`read`.

### Phase 2 — Commandes et sérialisation (terminée)

`save`/`load`/`reset`, format `.nn` v2, rejet explicite des
fichiers v1, tests de persistance (fichier tronqué, magie
corrompue, octets traînants).

### Phase 3 — Durcissement, tests, v1.0 (terminée)

Harnais maison minimal (macros `CHECK`, compteurs, zéro framework
externe, `make check`), portage C23/POSIX.1-2008 vérifié, build et
tests de bout en bout dans le sandbox QEMU de
[mistral-vm-debian-hurd](https://github.com/gnu-ai/mistral-vm-debian-hurd).

État courant (octobre 2026) : **v1.0, terminé** — `src` + `tests`.
Ce dépôt n'a plus de chantier ouvert : il sert de contrat de calcul
stable au reste de la pile ; la suite (plus gros modèles, GPU) se
joue côté système (voir le `WIP.md` de
[gnu-ai/hurd](https://github.com/gnu-ai/hurd)).

## 6. Jalons synthétiques

| Phase | Contenu | Dépend de | État |
|---|---|---|---|
| 0 | Spécification, contrats, format `.nn` | — | terminée |
| 1 | MVP : translator, arena, forward pass | 0 | terminée |
| 2 | `save`/`load`/`reset`, format v2, tests de persistance | 1 | terminée |
| 3 | Durcissement, harnais de tests, v1.0, CI Hurd | 1, 2 | terminée |

Ce translator est la brique la plus basse de la pile : tout ce qui
est au-dessus (orchestrateur, agrégation, votes) suppose que ce
contrat tient. Il évoluera par versions de format explicites
(`NET_FILE_VERSION`), jamais par changement silencieux.

Claire Ivanenka — claire@gnu-ai.org
GNU AI — https://gnu-ai.org
